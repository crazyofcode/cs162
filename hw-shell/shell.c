#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

static char *env_path_list;
/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_pwd (struct tokens* tokens);
int cmd_cd  (struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "prints the current working directory"},
    {cmd_cd , "cd", "changes the current working directory to that directory"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

int cmd_pwd (unused struct tokens* tokens) {
  char path[1024];
  getcwd(path, 1024);
  fprintf(stdout, "%s\n", path);
  return 1;
}

int cmd_cd  (struct tokens* tokens) {
  char *path = tokens_get_token(tokens, 1);
  return chdir(path);
}
/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

void redict(char *arg, int old_fd) {
  int fd = open(arg, O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    perror("open file error");
    exit(-1);
  }
  dup2(old_fd, fd);
  close(fd);
}

bool get_full_path(char *full_path, char *relative_path) {
  char env_path_list_back[1024];
  memcpy(env_path_list_back, env_path_list, strlen(env_path_list));
  char *svae_ptr;
  char *env_path = strtok_r(env_path_list_back, ":", &svae_ptr);
  strncpy(full_path, relative_path, strlen(relative_path));
  while(env_path != NULL) {
    if (access(full_path, X_OK) == 0) return true;
    snprintf(full_path, 256, "%s/%s", env_path, relative_path);
    env_path = strtok_r(NULL, ":", &svae_ptr);
  }
  return false;
}

int parseLine(char **argv, struct tokens *tokens, int idx) {
  char * relative_path = tokens_get_token(tokens, idx);
  char full_path[256];
  memset(full_path, 0, 256);
  if (!get_full_path(full_path, relative_path)) exit(-1);
  int index = 1;
  int i;
  bool flag = false;
  for (i = idx + 1; i < 64; i++) {
    char *arg;
    arg = tokens_get_token(tokens, i);
    if (arg == NULL) break;
    if (strncmp(arg, "|", 1) == 0) {
      flag = true;
      break;
    }
    if (strcmp(arg, ">") == 0) {
      arg = tokens_get_token(tokens, ++i);
      redict(arg, STDOUT_FILENO);
      continue;
    } else if (strcmp(arg, "<") == 0) {
      arg = tokens_get_token(tokens, ++i);
      redict(arg, STDIN_FILENO);
      continue;
    } else {
      argv[index++] = arg;
    }
  }
  size_t length = strlen(full_path);
  argv[0] = malloc(sizeof (char) * (length + 1));
  if (argv[0] == NULL)  {
    perror("malloc error");
    exit(-1);
  }
  strncpy(argv[0], full_path, length);
  argv[index] = NULL;
  // 如果 tokens 的数据已经读取完, 返回 0
  // 否则返回读到的 token 索引值
  if (flag)
    return i;
  else return 0;
}
int execute(char ***argv, int prog_num) {
  if (prog_num == 1)  return execv(argv[0][0], argv[0]);

  int pipe_arr[prog_num-1][2];  // 创建 prog_num - 1 个管道
  for (int i = 0; i < prog_num - 1; i++) {
    if (pipe(pipe_arr[i]) == -1) {
      perror("pipe error");
      exit(-1);
    }
  }

  pid_t pid;
  for (int i = 0; i < prog_num; i++) {
    pid = fork();
    if (pid == 0) {  // 子进程
      if (i == 0) {
        // 第一个进程：只需要重定向 stdout 到 pipe
        dup2(pipe_arr[i][1], STDOUT_FILENO);
      } else if (i == prog_num - 1) {
        // 最后一个进程：只需要重定向 stdin 到前一个 pipe
        dup2(pipe_arr[i-1][0], STDIN_FILENO);
      } else {
        // 中间进程：重定向 stdin 和 stdout
        dup2(pipe_arr[i-1][0], STDIN_FILENO);
        dup2(pipe_arr[i][1], STDOUT_FILENO);
      }

      // 关闭所有不需要的管道文件描述符
      for (int j = 0; j < prog_num - 1; j++) {
        close(pipe_arr[j][0]);
        close(pipe_arr[j][1]);
      }

      // 执行命令
      execv(argv[i][0], argv[i]);
      perror("execv error");  // execv 失败才会执行到这里
      exit(-1);
    } else if (pid < 0) {
      perror("fork error");
      return -1;
    }
  }

  // 父进程关闭所有管道文件描述符
  for (int i = 0; i < prog_num - 1; i++) {
    close(pipe_arr[i][0]);
    close(pipe_arr[i][1]);
  }

  // 等待所有子进程结束
  int state;
  for (int i = 0; i < prog_num; i++) {
    wait(&state);
  }
  return state;
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  env_path_list = getenv("PATH");

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    // for (int i = 0; i < tokens_get_length(tokens); i++) {
    //   printf("%s\n", tokens_get_token(tokens, i));
    // }
    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      // fprintf(stdout, "This shell doesn't know how to run programs.\n");
      pid_t pid = fork();
      if (pid == 0) {
        char **argv[8];
        int idx = 0;
        int prog_num = 0;
        for (int i = 0; i < 8; i++) {
          ++prog_num;
          argv[i] = malloc(sizeof(char **));
          int ret = parseLine(argv[i], tokens, idx);
          if (ret == 0) break;
          idx = ret + 1;
        }
        int state = execute(argv, prog_num);
        for (int i = 0; i > prog_num; i++) {
          free(argv[i][0]);
          free(argv[i]);
        }
        exit(state);
      } else if (pid > 0){
        int status;
        waitpid(pid, &status, 0);
      } else {
        fprintf(stderr, "this shell doesn't handler this problem.\n");
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
