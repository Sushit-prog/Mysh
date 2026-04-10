#ifndef MYSH_H
#define MYSH_H

/* ── Version ──────────────────────────────────────────────────────────────── */
#define MYSH_VERSION "0.1.0"
#define MYSH_NAME    "mysh"

/* ── Exit status codes ────────────────────────────────────────────────────── */
#define MYSH_OK       0   /* success                          */
#define MYSH_ERR      1   /* generic error                    */
#define MYSH_EXIT     2   /* shell should exit (e.g. exit cmd)*/
#define MYSH_MISUSE   2   /* bad usage / parse error          */

/* ── REPL input limits ────────────────────────────────────────────────────── */
#define MYSH_LINE_MAX  4096   /* max chars per input line         */
#define MYSH_ARG_MAX   1024   /* max tokens per command line      */

/* ── Shell global state ───────────────────────────────────────────────────── */
typedef struct {
    int   last_status;   /* $? — exit status of last command   */
    int   interactive;   /* 1 if stdin is a tty                */
    char *argv0;         /* name the shell was invoked as      */
} ShellState;

/* Global shell state — defined in src/init/config.c */
extern ShellState g_shell;

/* ── Sub-system entry points ──────────────────────────────────────────────── */
int shell_run(int argc, char **argv);   /* main REPL loop          */

#endif /* MYSH_H */