#include <readline/readline.h>
#include <readline/history.h>

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) {
  siglongjmp(loop_env, sig);
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
    if (mode == T_INPUT) {
      FILE *file = fopen(token[i], "r");
      *inputp = fileno(file);
      mode = NULL;

      for (int j = i; j < ntokens;
           j++) /* usuwamy z tablicy tokenów plik / przekierowanie */
        token[j] = token[j + 1];
    }

    else if (mode == T_OUTPUT) {
      FILE *file = fopen(token[i], "w");
      *outputp = fileno(file);
      mode = NULL;

      for (int j = i; j < ntokens; j++)
        token[j] = token[j + 1];

    }

    else if (token[i] == T_INPUT) {
      mode = T_INPUT;

      for (int j = i; j < ntokens; j++)
        token[j] = token[j + 1];

    }

    else if (token[i] == T_OUTPUT) {
      mode = T_OUTPUT;

      for (int j = i; j < ntokens; j++)
        token[j] = token[j + 1];
    }

    else {
      n++;
      /* Nie zmieniamy indeksu i ani ntokens, ponieważ nie dostaliśmy
       * przekierowania / pliku */
      i++;
      ntokens++;
    }

    i--;
    ntokens--;
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
  pid_t pid = Fork();

  if (!pid) {
    Sigprocmask(SIG_SETMASK, &mask, NULL);

    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);

    Setpgid(0, 0);

    if (output > -1)
      dup2(output, STDOUT_FILENO);

    MaybeClose(&output);

    if (input > -1)
      dup2(input, STDIN_FILENO);

    MaybeClose(&input);

    external_command(token);

  }

  else {

    Setpgid(pid, pid);

    int job = addjob(pid, bg);

    addproc(job, pid, token);

    if (bg)
      msg("[%d] running '%s'\n", job, jobcmd(job));

    else
      exitcode = monitorjob(&mask);
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();

  if (!pid) {
    Sigprocmask(SIG_SETMASK, mask, NULL);

    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);

    Setpgid(0, pgid);

    if (output > -1)
      dup2(output, STDOUT_FILENO);

    if (input > -1)
      dup2(input, STDIN_FILENO);

    if (builtin_command(token) >= 0)
      exit(0);

    external_command(token);
  }

  if (!pgid)
    setpgid(pid, pid);
  else
    setpgid(pid, pgid);

  MaybeClose(&input);
  MaybeClose(&output);

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */

  int lastInstruction = 0;
  int startingIndexInstruction = 0;

  for (int i = 0; i <= ntokens; i++) {
    if (i == ntokens) {

      MaybeClose(&output);
      MaybeClose(&next_input);
      lastInstruction = 1;
    }

    if (token[i] == T_PIPE || lastInstruction) {

      pid =
        do_stage(pgid, &mask, input, output, &token[startingIndexInstruction],
                 i - startingIndexInstruction);

      if (!pgid) {
        pgid = pid;
        job = addjob(pgid, bg);
      }

      addproc(job, pid, &token[startingIndexInstruction]);

      if (i == ntokens)
        break;

      startingIndexInstruction = i + 1;

      input = next_input;
      mkpipe(&next_input, &output);
    }
  }

  if (!bg)
    exitcode = monitorjob(&mask);

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(int argc, char *argv[]) {
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  Signal(SIGINT, sigint_handler);
  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  char *line;
  while (true) {
    if (!sigsetjmp(loop_env, 1)) {
      line = readline("# ");
    } else {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line)) {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
