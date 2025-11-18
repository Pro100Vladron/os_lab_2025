// launcher.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    // Проверка: должны быть переданы аргументы для sequential_min_max: seed и arraysize
    if (argc != 3) {
        fprintf(stderr, "Usage: %s seed arraysize\n", argv[0]);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return 1;
    } else if (pid == 0) {
        // === Дочерний процесс: запускаем sequential_min_max ===
        // Аргументы: sequential_min_max seed arraysize
        char *exec_argv[] = {
            "./sequential_min_max",
            argv[1],   // seed
            argv[2],   // arraysize
            NULL       // обязательный NULL-терминатор
        };

        execv("./sequential_min_max", exec_argv);

        // Если execv вернулся — ошибка
        perror("execv failed");
        exit(1);
    } else {
        // === Родительский процесс: ждём завершения дочернего ===
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            printf("sequential_min_max exited with code %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("sequential_min_max was killed by signal %d\n", WTERMSIG(status));
        } else {
            printf("sequential_min_max terminated abnormally\n");
        }
    }

    return 0;
}