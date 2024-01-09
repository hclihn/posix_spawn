#include <stdio.h>
#include <spawn.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <fcntl.h>

typedef enum {
    PROC_COM_INHERIT = 0,   // from parent
    PROC_COM_NONE,          // use /dev/null
    PROC_COM_PIPE,          // use pipe
    PROC_COM_FD,            // use supplied fd for stdXX
    PROC_COM_PATH,          // use supplied path for stdXX
    PROC_COM_STDOUT         // same as stdout; used for stderr only!
} ProcComType;

typedef struct {
    // input params
    ProcComType stdin_type;
    ProcComType stdout_type;
    ProcComType stderr_type;
    char* f_stdin;  // stdin file path
    char* f_stdout; // stdout file path
    char* f_stderr; // stderr file path
    // input (PROC_COM_FD) and/or output (PROC_COM_PIPE)
    int p_stdin;  // stdin pipe fd, negative if not used
    int p_stdout; // stdout pipe fd, negative if not used
    int p_stderr; // stderr pipe fd, negative if not used
    // output params
    pid_t pid;
} ProcInfo;

void showError(bool noop, char *fmt,...) {
    va_list args;

    fprintf(stderr, "ERROR: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt[strlen(fmt)-1] != '\n') {
        fprintf(stderr, "\n");
    }
}

void close_ProcInfo(ProcInfo *ci) {
    if (ci->p_stdin >= 0) {
        close(ci->p_stdin);
        ci->p_stdin = -1;
    }
    if (ci->p_stdout >= 0) {
        close(ci->p_stdout);
        ci->p_stdout = -1;
    }
    if (ci->p_stderr >= 0) {
        close(ci->p_stderr);
        ci->p_stderr = -1;
    }
    ci->pid = -1;
}

// create a subprocess and execute it
// args and env are char*[] with last element being NULL
int subprocess(ProcInfo *ci, char* args[], char* env[]) {
    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    int rc = 0;
    posix_spawn_file_actions_t action;

    posix_spawn_file_actions_init(&action);
    switch (ci->stdin_type) {
        case PROC_COM_PIPE:
            if (pipe(stdin_pipe)) {
                showError(false, "Failed to create stdin pipe for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            // on the child side
            posix_spawn_file_actions_addclose(&action, stdin_pipe[1]); // the write end
            posix_spawn_file_actions_adddup2(&action, stdin_pipe[0], STDIN_FILENO); // the read end
            posix_spawn_file_actions_addclose(&action, stdin_pipe[0]);
            break;
        case PROC_COM_FD:
            if (ci->p_stdin < 0) {
                showError(false, "Invalid stdin fd (%d) for subprocess %s: %s!",
                    ci->p_stdin, args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_adddup2(&action, ci->p_stdin, STDIN_FILENO);
            if (ci->p_stdin != STDIN_FILENO) {
              posix_spawn_file_actions_addclose(&action, ci->p_stdin);
            }
            break;
        case PROC_COM_STDOUT:
            showError(false, "Invalid pipe type (PROC_COM_STDOUT) for stdin!");
            rc = 1;
            goto clean_up;
        case PROC_COM_PATH:
            if (ci->f_stdin == NULL) {
                showError(false, "Empty stdin path for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_addopen(&action, STDIN_FILENO, ci->f_stdin, O_RDONLY, 0644);
            break;
        case PROC_COM_NONE:
            posix_spawn_file_actions_addclose(&action, STDIN_FILENO);
            break;
        case PROC_COM_INHERIT: // no-op
            break;
        default:
            showError(false, "Unknown PipeType (%d) for stdin!", ci->stdin_type);
            rc = 1;
            goto clean_up;
    }
    switch (ci->stdout_type) {
        case PROC_COM_PIPE:
            if (pipe(stdout_pipe)) {
                showError(false, "Failed to create stdout pipe for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            // on the child side
            posix_spawn_file_actions_addclose(&action, stdout_pipe[0]); // the read end
            posix_spawn_file_actions_adddup2(&action, stdout_pipe[1], STDOUT_FILENO); // the write end
            posix_spawn_file_actions_addclose(&action, stdout_pipe[1]);
            break;
        case PROC_COM_STDOUT:
            showError(false, "Invalid pipe type (PROC_COM_STDOUT) for stdout!");
            rc = 1;
            goto clean_up;
        case PROC_COM_FD:
            if (ci->p_stdout < 0) {
                showError(false, "Invalid stdout fd (%d) for subprocess %s: %s!",
                    ci->p_stdout, args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_adddup2(&action, ci->p_stdout, STDOUT_FILENO);
            if (ci->p_stdout != STDOUT_FILENO) {
              posix_spawn_file_actions_addclose(&action, ci->p_stdout);
            }
            break;
        case PROC_COM_PATH:
            if (ci->f_stdout == NULL) {
                showError(false, "Empty stdout path for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, ci->f_stdout, O_CREAT|O_WRONLY|O_TRUNC, 0644);
            break;
        case PROC_COM_NONE:
            posix_spawn_file_actions_addclose(&action, STDOUT_FILENO);
            break;
        case PROC_COM_INHERIT: // no-op
            break;
        default:
            showError(false, "Unknown PipeType (%d) for stdout!", ci->stdout_type);
            rc = 1;
            goto clean_up;
    }
    switch (ci->stderr_type) {
        case PROC_COM_PIPE:
            if (pipe(stderr_pipe)) {
                showError(false, "Failed to create stderr pipe for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            // on the child side
            posix_spawn_file_actions_addclose(&action, stderr_pipe[0]); // the read end
            posix_spawn_file_actions_adddup2(&action, stderr_pipe[1], STDERR_FILENO); // the write end
            posix_spawn_file_actions_addclose(&action, stderr_pipe[1]);
            break;
        case PROC_COM_FD:
            if (ci->p_stderr < 0) {
                showError(false, "Invalid stderr fd (%d) for subprocess %s: %s!",
                    ci->p_stderr, args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_adddup2(&action, ci->p_stderr, STDERR_FILENO);
            if (ci->p_stderr != STDERR_FILENO) {
              posix_spawn_file_actions_addclose(&action, ci->p_stderr);
            }
            break;
        case PROC_COM_PATH:
            if (ci->f_stderr == NULL) {
                showError(false, "Empty stderr path for subprocess %s: %s!", args[0], strerror(errno));
                rc = 1;
                goto clean_up;
            }
            posix_spawn_file_actions_addopen(&action, STDERR_FILENO, ci->f_stderr, O_CREAT|O_WRONLY|O_TRUNC, 0644);
            break;
        case PROC_COM_STDOUT:
            if (ci->stdout_type == PROC_COM_INHERIT) { // no-op
                break;
            } else if (ci->stdout_type != PROC_COM_NONE) {
                posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);
                break;
            }
            // fall thorugh for ci->stdout_type == PROC_COM_NONE
        case PROC_COM_NONE:
            posix_spawn_file_actions_addclose(&action, STDERR_FILENO);
            break;
        case PROC_COM_INHERIT: // no-op
            break;
        default:
            showError(false, "Unknown PipeType (%d) for stderr!", ci->stderr_type);
            rc = 1;
            goto clean_up;
    }

    rc = posix_spawnp(&(ci->pid), args[0], &action, NULL, args, env);
    if(rc != 0) {
        showError(false, "Failed to spawn subprocess %s: %s!", args[0], strerror(errno));
        goto clean_up;
    }
    // close child-side of pipes, and assign returned pipes
    if (ci->stdin_type == PROC_COM_PIPE) {
        close(stdin_pipe[0]); // the read end
        ci->p_stdin = stdin_pipe[1]; // the write end
    } else if (ci->stdin_type != PROC_COM_FD) {
        ci->p_stdin = -1;
    }
    if (ci->stdout_type == PROC_COM_PIPE) {
        close(stdout_pipe[1]); // the write end
        ci->p_stdout = stdout_pipe[0]; // the read end
    } else if (ci->stdin_type != PROC_COM_FD) {
        ci->p_stdout = -1;
    }
    if (ci->stderr_type == PROC_COM_PIPE) {
        close(stderr_pipe[1]); // the write end
        ci->p_stderr = stderr_pipe[0]; // the read end
    } else if (ci->stdin_type != PROC_COM_FD) {
        // When PROC_COM_STDOUT is used, the caller should use just ci->p_stdout
        ci->p_stderr = -1;
    }

clean_up:
    posix_spawn_file_actions_destroy(&action);
    return rc;
}

int main(void) {
  ProcInfo ci = {.p_stdin=-1, .p_stdout=-1, .p_stderr=-1,
    .stdout_type=PROC_COM_FD, .stderr_type=PROC_COM_PIPE};
  ProcInfo ci2 = {.p_stdin=-1, .p_stdout=-1, .p_stderr=-1,
    //.f_stderr="wc_out.txt",
    .stdin_type=PROC_COM_PIPE, .stdout_type=PROC_COM_PIPE, 
    .stderr_type=PROC_COM_STDOUT};
  char* args[] = {"ls", "/bin", NULL};
  char* args2[] = {"wc", NULL};
  int exit_code;
  printf("Hello World\n");
  subprocess(&ci2, args2, NULL);
  ci.p_stdout = ci2.p_stdin;
  //printf("stdout fd: %d\n", ci.p_stdout);
  subprocess(&ci, args, NULL);
  char buffer[128];
  struct pollfd plist[] = { {ci.p_stderr, POLLIN} };
  int p_sz = sizeof(plist) / sizeof(struct pollfd);
  int br;
  for (int rval; (rval=poll(plist, p_sz, -1)) > 0; ) {
    if (plist[0].revents & POLLIN) {
      br = read(ci.p_stderr, buffer, sizeof(buffer)-1);
      printf("-> read %d bytes from %s stderr:\n", br, args[0]);
      buffer[br] = '\0';
      printf("%s\n", buffer);
    } else {
      break; // nothing left to read
    }
  }
  waitpid(ci.pid, &exit_code, 0);
  printf("Done %d\n", exit_code);
  // close the stiin pipe to allow wc to end!
  close(ci2.p_stdin);
  ci2.p_stdin = -1;
  struct pollfd plist2[] = { {ci2.p_stdout, POLLIN} };
  int p2_sz = sizeof(plist2) / sizeof(struct pollfd);
  for (int rval; (rval=poll(plist2, p2_sz, -1)) > 0; ) {
    if (plist2[0].revents & POLLIN) {
      int br = read(ci2.p_stdout, buffer, sizeof(buffer)-1);
      printf("-> read %d bytes from %s stdout:\n", br, args2[0]);
      buffer[br] = '\0';
      printf("%s\n", buffer);
    } else {
      break; // nothing left to read
    }
  }
  waitpid(ci2.pid, &exit_code, 0);
  printf("Done2 %d\n", exit_code);
  close_ProcInfo(&ci);
  close_ProcInfo(&ci2);

  ProcInfo ci3 = {.p_stdin=-1, .p_stdout=-1, .p_stderr=-1,
    .f_stdin="wc_out.txt", .stdin_type=PROC_COM_PATH};
  subprocess(&ci3, args2, NULL);
  waitpid(ci3.pid, &exit_code, 0);
  printf("Done3 %d\n", exit_code);
  close_ProcInfo(&ci3);

  printf("snprintf: %d\n", snprintf(NULL, 0, "This is a test %d!", exit_code));
  return 0;
}