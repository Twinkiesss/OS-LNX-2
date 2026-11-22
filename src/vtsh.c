
#define _GNU_SOURCE

#include "vtsh.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_STACK_SIZE (1024 * 1024)
#define DEFAULT_NPROC_LIMIT 64
#define MAX_TOKENS 256

typedef struct Job {
    pid_t pid;
    struct Job *next;
} Job;

typedef struct {
    char **argv;
    rlim_t nproc_limit;
} ChildData;

static Job *background_jobs = NULL;

/*
 * Читает лимит процессов из переменной окружения.
 * Если переменная не задана, используется значение по умолчанию.
 */
static int get_nproc_limit(rlim_t *limit) {
    const char *value = getenv("VTSH_NPROC_LIMIT");

    if (value == NULL || *value == '\0') {
        *limit = DEFAULT_NPROC_LIMIT;
        return 0;
    }

    if (*value == '-' || *value == '+') {
        fprintf(stderr, "vtsh: invalid VTSH_NPROC_LIMIT value\n");
        return -1;
    }

    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "vtsh: invalid VTSH_NPROC_LIMIT value\n");
            return -1;
        }
    }

    char *end = NULL;
    errno = 0;

    unsigned long long parsed = strtoull(value, &end, 10);

    if (errno == ERANGE || end == value || *end != '\0') {
        fprintf(stderr, "vtsh: invalid VTSH_NPROC_LIMIT value\n");
        return -1;
    }

    rlim_t converted = (rlim_t) parsed;

    if ((unsigned long long) converted != parsed) {
        fprintf(stderr, "vtsh: VTSH_NPROC_LIMIT is too large\n");
        return -1;
    }

    *limit = converted;
    return 0;
}

/*
 * Функция дочернего процесса.
 * Устанавливает RLIMIT_NPROC и запускает внешнюю программу.
 */
static int child_process(void *argument) {
    ChildData *data = argument;

    struct rlimit limit;

    if (getrlimit(RLIMIT_NPROC, &limit) == -1) {
        perror("vtsh: getrlimit");
        _exit(125);
    }

    limit.rlim_cur = data->nproc_limit;

    if (setrlimit(RLIMIT_NPROC, &limit) == -1) {
        perror("vtsh: setrlimit");
        _exit(125);
    }

    execvp(data->argv[0], data->argv);

    int exec_error = errno;

    if (exec_error == ENOENT) {
        printf("Command not found\n");
        fflush(stdout);
    } else {
        errno = exec_error;
        perror("vtsh: exec");
    }

    _exit(exec_error == ENOENT ? 127 : 126);
}

/* Удаляет завершённое задание из списка фоновых процессов. */
static void remove_job(pid_t pid) {
    Job **current = &background_jobs;

    while (*current != NULL) {
        if ((*current)->pid == pid) {
            Job *removed = *current;
            *current = removed->next;
            free(removed);
            return;
        }

        current = &(*current)->next;
    }
}

/*
 * Собирает завершившиеся фоновые процессы,
 * не блокируя выполнение оболочки.
 */
static void reap_background_jobs(void) {
    int status;
    pid_t pid;

    while (1) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            remove_job(pid);
            continue;
        }

        if (pid == -1 && errno == EINTR) {
            continue;
        }

        break;
    }
}

/* Добавляет PID фонового процесса в список заданий. */
static int add_background_job(pid_t pid) {
    Job *job = malloc(sizeof(Job));

    if (job == NULL) {
        perror("vtsh: malloc");
        return -1;
    }

    job->pid = pid;
    job->next = background_jobs;
    background_jobs = job;

    return 0;
}

/* Преобразует статус waitpid в код завершения программы. */
static int process_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

/* Ожидает конкретный дочерний процесс с обработкой EINTR. */
static int wait_for_process(pid_t pid) {
    int status;

    while (1) {
        pid_t result = waitpid(pid, &status, 0);

        if (result == pid) {
            return process_status(status);
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        perror("vtsh: waitpid");
        return 1;
    }
}

/*
 * Создаёт отдельный процесс с помощью clone().
 * Для дочернего процесса выделяется отдельный стек.
 */
static int launch_program(char **argv, int background) {
    rlim_t nproc_limit;

    if (get_nproc_limit(&nproc_limit) != 0) {
        return 1;
    }

    void *stack = malloc(CHILD_STACK_SIZE);

    if (stack == NULL) {
        perror("vtsh: malloc");
        return 1;
    }

    ChildData data = {
        .argv = argv,
        .nproc_limit = nproc_limit
    };

    void *stack_top = (char *) stack + CHILD_STACK_SIZE;

    pid_t pid = clone(
        child_process,
        stack_top,
        SIGCHLD,
        &data
    );

    if (pid == -1) {
        perror("vtsh: clone");
        free(stack);
        return 1;
    }

    free(stack);

    if (background) {
        if (add_background_job(pid) != 0) {
            wait_for_process(pid);
            return 1;
        }

        return 0;
    }

    return wait_for_process(pid);
}

/* Встроенная команда cd выполняется в самой оболочке. */
static int execute_cd(char **argv) {
    if (argv[1] == NULL) {
        fprintf(stderr, "vtsh: cd: expected directory\n");
        return 1;
    }

    if (argv[2] != NULL) {
        fprintf(stderr, "vtsh: cd: too many arguments\n");
        return 1;
    }

    if (chdir(argv[1]) == -1) {
        perror("vtsh: cd");
        return 1;
    }

    return 0;
}

/* Выполняет одну команду. */
static int execute_command(
    char **argv,
    int background,
    int *should_exit
) {
    if (argv[0] == NULL) {
        return 0;
    }

    if (strcmp(argv[0], "exit") == 0) {
        *should_exit = 1;
        return 0;
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (background) {
            fprintf(stderr, "vtsh: cd cannot run in background\n");
            return 1;
        }

        return execute_cd(argv);
    }

    return launch_program(argv, background);
}

/*
 * Проверяет синтаксис последовательности команд.
 * Операторы ; и & должны быть отдельными токенами.
 */
static int validate_syntax(char **tokens, int token_count) {
    int position = 0;

    if (token_count == 0) {
        return 0;
    }

    while (position < token_count) {
        if (strcmp(tokens[position], ";") == 0 ||
            strcmp(tokens[position], "&") == 0) {
            fprintf(stderr, "vtsh: syntax error\n");
            return -1;
        }

        while (position < token_count &&
               strcmp(tokens[position], ";") != 0 &&
               strcmp(tokens[position], "&") != 0) {
            position++;
        }

        if (position < token_count) {
            position++;

            if (position < token_count &&
                (strcmp(tokens[position], ";") == 0 ||
                 strcmp(tokens[position], "&") == 0)) {
                fprintf(stderr, "vtsh: syntax error\n");
                return -1;
            }
        }
    }

    return 0;
}

/* Разбирает строку и последовательно выполняет команды. */
static int execute_line(char *line, int *should_exit) {
    char *tokens[MAX_TOKENS];
    int token_count = 0;

    char *saveptr = NULL;
    char *token = strtok_r(line, " \t\r\n", &saveptr);

    while (token != NULL) {
        if (token_count >= MAX_TOKENS - 1) {
            fprintf(stderr, "vtsh: too many arguments\n");
            return 1;
        }

        tokens[token_count++] = token;
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    tokens[token_count] = NULL;

    if (token_count == 0) {
        return 0;
    }

    if (validate_syntax(tokens, token_count) != 0) {
        return 1;
    }

    int position = 0;
    int last_status = 0;

    while (position < token_count) {
        int command_start = position;

        while (position < token_count &&
               strcmp(tokens[position], ";") != 0 &&
               strcmp(tokens[position], "&") != 0) {
            position++;
        }

        char *separator = NULL;

        if (position < token_count) {
            separator = tokens[position];
            tokens[position] = NULL;
            position++;
        }

        int background = separator != NULL &&
                         strcmp(separator, "&") == 0;

        last_status = execute_command(
            &tokens[command_start],
            background,
            should_exit
        );

        if (*should_exit) {
            break;
        }
    }

    return last_status;
}

/* Главный цикл оболочки. */
int vtsh_run(void) {
    char *line = NULL;
    size_t capacity = 0;
    int should_exit = 0;

    /*
     * Отключаем буферизацию stdin, чтобы дочерние программы
     * могли читать оставшийся ввод, например команда cat.
     */
    setvbuf(stdin, NULL, _IONBF, 0);

    int interactive = isatty(STDIN_FILENO);

    while (!should_exit) {
        reap_background_jobs();

        if (interactive) {
            printf("vtsh> ");
            fflush(stdout);
        }

        ssize_t length = getline(&line, &capacity, stdin);

        if (length == -1) {
            break;
        }

        execute_line(line, &should_exit);
    }

    free(line);

    /*
     * При завершении оболочки незавершённые фоновые процессы
     * не ожидаются. Они переподчиняются системе.
     * Завершившиеся процессы собираются во время работы
     * оболочки с помощью waitpid(..., WNOHANG).
     */
    while (background_jobs != NULL) {
        Job *next = background_jobs->next;
        free(background_jobs);
        background_jobs = next;
    }

    return 0;
}
