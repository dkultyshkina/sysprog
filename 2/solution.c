#include "parser.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int execute_cd(struct expr *iterator) {
  if (iterator->cmd.arg_count != 0) {
    if (chdir(iterator->cmd.args[0]) != 0) {
      exit(1);
    }
  } else {
    const char *home = getenv("HOME");
    if (home == NULL) {
      exit(1);
    }
    if (chdir(home) != 0) {
      exit(1);
    }
  }
  return 0;
}

int count_number_command(const struct command_line *line) {
  int number_commands = 0;
  struct expr *iterator = line->head;
  while (iterator != NULL) {
    number_commands++;
    iterator = iterator->next;
  }
  return number_commands;
}

static int execute_command_line(const struct command_line *line) {
  assert(line != NULL);
  int result = 0;
  int number_commands = count_number_command(line);
  struct expr *iterator = line->head;
  int fd[2];
  int prev_pipe_read = STDIN_FILENO;
  for (int i = 0; (i < number_commands) && (iterator != NULL); i++) {
    if (iterator->type != EXPR_TYPE_COMMAND) {
      iterator = iterator->next;
      continue;
    }
    if (!strcmp(iterator->cmd.exe, "cd")) {
      result = execute_cd(iterator);
      return result;
    } else if (!strcmp(iterator->cmd.exe, "exit") && (number_commands == 1)) {
      int arg = 0;
      if (iterator->cmd.arg_count != 0) {
        arg = atoi(*iterator->cmd.args);
      }
      exit(arg);
    } else {
      if (!strcmp(iterator->cmd.exe, "exit")) {
        int arg = 0;
        if (iterator->cmd.arg_count != 0) {
          arg = atoi(*iterator->cmd.args);
        }
        result = arg;
      }
      if (i < number_commands - 1) {
        if (pipe(fd) == -1) {
          exit(1);
        }
      }
      pid_t pid = fork();
      if (pid == -1) {
        exit(1);
      } else if (pid == 0) {
        if (i > 0) {
          if (dup2(prev_pipe_read, STDIN_FILENO) == -1) {
            exit(1);
          }
          close(prev_pipe_read);
        }
        if (i < (number_commands - 1)) {
          if (dup2(fd[1], STDOUT_FILENO) == -1) {
            exit(1);
          }
          close(fd[1]);
        }
        if (line->out_type == OUTPUT_TYPE_FILE_NEW &&
            (i == (number_commands - 1))) {
          fd[1] = open(line->out_file, O_TRUNC | O_CREAT | O_WRONLY, 0644);
          if (fd[1] == -1) {
            exit(1);
          }
          if (dup2(fd[1], STDOUT_FILENO) == -1) {
            exit(1);
          }
          close(fd[1]);
        } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
          fd[1] = open(line->out_file, O_APPEND | O_CREAT | O_WRONLY, 0644);
          if (fd[1] == -1) {
            exit(1);
          }
          if (dup2(fd[1], STDOUT_FILENO) == -1) {
            exit(1);
          }
          close(fd[1]);
        }
        if (i > 0) {
          close(prev_pipe_read);
        }
        if (i < number_commands - 1) {
          close(fd[0]);
        }
        char **args = calloc(iterator->cmd.arg_count + 2, sizeof(char *));
        args[0] = iterator->cmd.exe;
        memcpy(args + 1, iterator->cmd.args,
               sizeof(char *) * iterator->cmd.arg_count);
        result = execvp(iterator->cmd.exe, args);
        if (result == -1) {
          exit(1);
        }
      } else {
        if (i > 0) {
          close(prev_pipe_read);
        }
        if (i < (number_commands - 1)) {
          close(fd[1]);
        }
        if (i < (number_commands - 1)) {
          prev_pipe_read = fd[0];
        }
      }
      iterator = iterator->next;
    }
  }
  for (int i = 0; i < number_commands; i++) {
    wait(NULL);
  }
  return result;
}

int main(void) {
  const size_t buf_size = 1024;
  char buf[buf_size];
  int rc;
  struct parser *p = parser_new();
  int result = 0;
  while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
    parser_feed(p, buf, rc);
    struct command_line *line = NULL;
    while (true) {
      enum parser_error err = parser_pop_next(p, &line);
      if (err == PARSER_ERR_NONE && line == NULL)
        break;
      if (err != PARSER_ERR_NONE) {
        printf("Error: %d\n", (int)err);
        continue;
      }
      result = execute_command_line(line);
      command_line_delete(line);
    }
  }
  parser_delete(p);
  return result;
}
