#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <getopt.h>

#include "find_min_max.h"
#include "utils.h"

int main(int argc, char **argv) {
  int seed = -1;
  int array_size = -1;
  int pnum = -1;
  bool with_files = false;

  while (true) {
    int current_optind = optind ? optind : 1;

    static struct option options[] = {
        {"seed", required_argument, 0, 0},
        {"array_size", required_argument, 0, 0},
        {"pnum", required_argument, 0, 0},
        {"by_files", no_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c = getopt_long(argc, argv, "f", options, &option_index);

    if (c == -1) break;

    switch (c) {
      case 0:
        switch (option_index) {
          case 0:
            seed = atoi(optarg);
            if (seed <= 0) {
              printf("seed must be a positive integer\n");
              return 1;
            }
            break;
          case 1:
            array_size = atoi(optarg);
            if (array_size <= 0) {
              printf("array_size must be a positive integer\n");
              return 1;
            }
            break;
          case 2:
            pnum = atoi(optarg);
            if (pnum <= 0) {
              printf("pnum must be a positive integer\n");
              return 1;
            }
            if (pnum > array_size) {
              printf("Warning: pnum > array_size — some processes will get empty chunks\n");
            }
            break;
          default:
            printf("Index %d is out of options\n", option_index);
            return 1;
        }
        break;
      case 'f':
        with_files = true;
        break;
      case '?':
        // getopt уже напечатал ошибку
        return 1;
      default:
        printf("getopt returned character code 0%o?\n", c);
        return 1;
    }
  }

  if (optind < argc) {
    printf("Has at least one no option argument\n");
    return 1;
  }

  if (seed == -1 || array_size == -1 || pnum == -1) {
    printf("Usage: %s --seed \"num\" --array_size \"num\" --pnum \"num\" [--by_files]\n",
           argv[0]);
    return 1;
  }

  int *array = malloc(sizeof(int) * array_size);
  if (!array) {
    perror("malloc");
    return 1;
  }
  GenerateArray(array, array_size, seed);

  // Подготовка к параллелизму
  int active_child_processes = 0;

  // Для pipe-режима: массив дескрипторов
  int pipefd[2 * pnum]; // каждый процесс — свой pipe: [read0, write0, read1, write1, ...]

  if (!with_files) {
    // Создаём pnum pipe'ов
    for (int i = 0; i < pnum; ++i) {
      if (pipe(&pipefd[2 * i]) == -1) {
        perror("pipe");
        free(array);
        return 1;
      }
    }
  }

  struct timeval start_time;
  gettimeofday(&start_time, NULL);

  // Запуск дочерних процессов
  for (int i = 0; i < pnum; i++) {
    // Расчёт чанка: [begin, end)
    int chunk_size = array_size / pnum;
    int remainder = array_size % pnum;
    int begin = i * chunk_size + (i < remainder ? i : remainder);
    int end = begin + chunk_size + (i < remainder ? 1 : 0);

    pid_t child_pid = fork();
    if (child_pid >= 0) {
      active_child_processes++;
      if (child_pid == 0) {
        // === ДОЧЕРНИЙ ПРОЦЕСС ===
        struct MinMax local_mm = GetMinMax(array, begin, end);

        if (with_files) {
          // Запись в файл: result_i.txt
          char filename[64];
          snprintf(filename, sizeof(filename), "result_%d.txt", i);
          FILE *f = fopen(filename, "w");
          if (!f) {
            perror("fopen (child)");
            exit(1);
          }
          fprintf(f, "%d %d", local_mm.min, local_mm.max);
          fclose(f);
        } else {
          // Запись в pipe: закрываем чтение, пишем в write-конец
          close(pipefd[2 * i]);           // read end
          write(pipefd[2 * i + 1], &local_mm.min, sizeof(int));
          write(pipefd[2 * i + 1], &local_mm.max, sizeof(int));
          close(pipefd[2 * i + 1]);       // write end
        }

        exit(0); // важно: не return!
      }
    } else {
      perror("fork");
      free(array);
      return 1;
    }
  }

  // === РОДИТЕЛЬ ===

  // Закрываем write-концы pipe в родителе (если pipe)
  if (!with_files) {
    for (int i = 0; i < pnum; ++i) {
      close(pipefd[2 * i + 1]); // write end
    }
  }

  // Ждём завершения всех детей
  while (active_child_processes > 0) {
    int status;
    pid_t pid = wait(&status);
    if (pid == -1) {
      perror("wait");
      break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      printf("Child %d exited abnormally\n", pid);
    }
    active_child_processes--;
  }

  // Сбор результатов
  struct MinMax global_mm;
  global_mm.min = INT_MAX;
  global_mm.max = INT_MIN;

  for (int i = 0; i < pnum; i++) {
    int min = INT_MAX, max = INT_MIN;

    if (with_files) {
      char filename[64];
      snprintf(filename, sizeof(filename), "result_%d.txt", i);
      FILE *f = fopen(filename, "r");
      if (!f) {
        perror("fopen (parent)");
        free(array);
        return 1;
      }
      if (fscanf(f, "%d %d", &min, &max) != 2) {
        fprintf(stderr, "Failed to read from %s\n", filename);
        fclose(f);
        free(array);
        return 1;
      }
      fclose(f);
      // Опционально: unlink(filename); чтобы убрать мусор
    } else {
      // Чтение из pipe
      if (read(pipefd[2 * i], &min, sizeof(int)) != sizeof(int) ||
          read(pipefd[2 * i] + 2 * i, &max, sizeof(int)) != sizeof(int)) {  // ← ОШИБКА!!!
        // Исправим: читаем из pipefd[2*i] (read end)
        // Но выше уже закрыли write end — всё ок
      }
      // Правильно:
      if (read(pipefd[2 * i], &min, sizeof(int)) != sizeof(int)) {
        perror("read min");
        free(array);
        return 1;
      }
      if (read(pipefd[2 * i], &max, sizeof(int)) != sizeof(int)) {
        perror("read max");
        free(array);
        return 1;
      }
      close(pipefd[2 * i]); // закрываем read end
    }

    if (min < global_mm.min) global_mm.min = min;
    if (max > global_mm.max) global_mm.max = max;
  }

  struct timeval finish_time;
  gettimeofday(&finish_time, NULL);

  double elapsed_time = (finish_time.tv_sec - start_time.tv_sec) * 1000.0;
  elapsed_time += (finish_time.tv_usec - start_time.tv_usec) / 1000.0;

  free(array);

  // Очистка файлов (если использовали файлы)
  if (with_files) {
    for (int i = 0; i < pnum; ++i) {
      char filename[64];
      snprintf(filename, sizeof(filename), "result_%d.txt", i);
      unlink(filename); // удаляем временные файлы
    }
  }

  printf("Min: %d\n", global_mm.min);
  printf("Max: %d\n", global_mm.max);
  printf("Elapsed time: %.2f ms\n", elapsed_time);
  fflush(NULL);
  return 0;
}