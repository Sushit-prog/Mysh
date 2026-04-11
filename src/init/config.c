#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>

#include "mysh.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "expand.h"

/* ── Global shell state ───────────────────────────────────────────────────── */
ShellState g_shell = {
    .last_status = 0,
    .interactive = 0,
    .argv0       = NULL,
    .pgid        = 0,
    .job_count   = 0,
};

/* ── Signal handling ──────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_got_sigint  = 0;
static volatile sig_atomic_t g_got_sigchld = 0;

static void sigint_handler(int sig)  { (void)sig; g_got_sigint  = 1; }
static void sigchld_handler(int sig) { (void)sig; g_got_sigchld = 1; }

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    /* Shell ignores these in interactive mode */
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
}

/* ── Prompt ───────────────────────────────────────────────────────────────── */

static void print_prompt(void)
{
    if (g_shell.interactive) {
        fprintf(stderr, "mysh> ");
        fflush(stderr);
    }
}

/* ── Input reading ────────────────────────────────────────────────────────── */

static char *read_line(FILE *fp)
{
    size_t cap = 128, len = 0;
    char  *buf = malloc(cap);
    if (!buf) { perror("mysh: malloc"); return NULL; }

    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { perror("mysh: realloc"); free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }

    if (len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* ── Heredoc reading ──────────────────────────────────────────────────────── */

/*
 * Read lines from stdin until a line matching `delim` is seen.
 * Returns heap-allocated string of all lines, or NULL on error.
 */
char *read_heredoc(const char *delim)
{
    size_t cap = 256, len = 0;
    char  *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        if (g_shell.interactive) {
            fprintf(stderr, "> ");
            fflush(stderr);
        }
        char *line = read_line(stdin);
        if (!line) break; /* EOF */

        /* Strip trailing newline for comparison */
        size_t llen = strlen(line);
        if (llen > 0 && line[llen-1] == '\n') line[llen-1] = '\0';

        if (strcmp(line, delim) == 0) { free(line); break; }

        /* Re-add newline and append */
        line[llen-1] = '\n'; /* restore */
        size_t needed = len + llen + 1;
        if (needed >= cap) {
            while (cap < needed) cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(line); free(buf); return NULL; }
            buf = tmp;
        }
        memcpy(buf + len, line, llen);
        len += llen;
        free(line);
    }

    buf[len] = '\0';
    return buf;
}

/* ── Builtins ─────────────────────────────────────────────────────────────── */

static void handle_builtin_exit(char **argv)
{
    int code = g_shell.last_status;
    if (argv[1]) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end == '\0') code = (int)(v & 0xFF);
        else fprintf(stderr, "mysh: exit: numeric argument required\n");
    }
    exit(code);
}

static int handle_builtin_cd(char **argv)
{
    const char *dir = argv[1] ? argv[1] : getenv("HOME");
    if (!dir) { fprintf(stderr, "mysh: cd: HOME not set\n"); return 1; }
    if (chdir(dir) < 0) {
        fprintf(stderr, "mysh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char *cwd = getcwd(NULL, 0);
    if (cwd) { setenv("PWD", cwd, 1); free(cwd); }
    return 0;
}

static int handle_builtin_jobs(void)
{
    for (int i = 0; i < MYSH_MAX_JOBS; i++) {
        Job *j = &g_shell.jobs[i];
        if (j->id == 0) continue;
        const char *state = j->state == JOB_RUNNING ? "Running" :
                            j->state == JOB_STOPPED  ? "Stopped" : "Done";
        fprintf(stderr, "[%d] %s\t\t%s\n", j->id, state,
                j->cmdline ? j->cmdline : "");
    }
    return 0;
}

static int handle_builtin_fg(char **argv)
{
    int id = 0;
    if (argv[1]) id = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
    else {
        /* Find most recent job */
        for (int i = MYSH_MAX_JOBS - 1; i >= 0; i--)
            if (g_shell.jobs[i].id != 0) { id = g_shell.jobs[i].id; break; }
    }
    Job *j = job_find_id(id);
    if (!j) { fprintf(stderr, "mysh: fg: %d: no such job\n", id); return 1; }

    /* Give terminal to job */
    if (g_shell.interactive)
        tcsetpgrp(STDIN_FILENO, j->pgid);

    /* Continue if stopped */
    if (j->state == JOB_STOPPED)
        killpg(j->pgid, SIGCONT);

    j->state = JOB_RUNNING;

    /* Wait for it */
    int ws, status = 0;
    while (waitpid(-j->pgid, &ws, WUNTRACED) > 0) {
        if (WIFSTOPPED(ws)) {
            j->state = JOB_STOPPED;
            fprintf(stderr, "\n[%d] Stopped\t\t%s\n", j->id,
                    j->cmdline ? j->cmdline : "");
            break;
        }
        if (WIFEXITED(ws) || WIFSIGNALED(ws)) {
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
            job_remove(j->id);
            break;
        }
    }

    /* Take terminal back */
    if (g_shell.interactive)
        tcsetpgrp(STDIN_FILENO, g_shell.pgid);

    return status;
}

static int handle_builtin_bg(char **argv)
{
    int id = 0;
    if (argv[1]) id = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
    else {
        for (int i = MYSH_MAX_JOBS - 1; i >= 0; i--)
            if (g_shell.jobs[i].id != 0 &&
                g_shell.jobs[i].state == JOB_STOPPED)
                { id = g_shell.jobs[i].id; break; }
    }
    Job *j = job_find_id(id);
    if (!j) { fprintf(stderr, "mysh: bg: %d: no such job\n", id); return 1; }
    j->state = JOB_RUNNING;
    killpg(j->pgid, SIGCONT);
    fprintf(stderr, "[%d] %s\n", j->id, j->cmdline ? j->cmdline : "");
    return 0;
}

static int handle_builtin_kill(char **argv)
{
    if (!argv[1]) { fprintf(stderr, "mysh: kill: usage: kill [-SIG] %%job|pid\n"); return 1; }
    int sig = SIGTERM;
    int i = 1;
    if (argv[i][0] == '-') {
        sig = atoi(argv[i] + 1);
        if (sig == 0) {
            /* Try signal name */
            if      (strcmp(argv[i]+1, "KILL") == 0) sig = SIGKILL;
            else if (strcmp(argv[i]+1, "INT")  == 0) sig = SIGINT;
            else if (strcmp(argv[i]+1, "TERM") == 0) sig = SIGTERM;
            else if (strcmp(argv[i]+1, "STOP") == 0) sig = SIGSTOP;
            else if (strcmp(argv[i]+1, "CONT") == 0) sig = SIGCONT;
            else { fprintf(stderr, "mysh: kill: unknown signal %s\n", argv[i]); return 1; }
        }
        i++;
    }
    if (!argv[i]) { fprintf(stderr, "mysh: kill: missing target\n"); return 1; }
    for (; argv[i]; i++) {
        if (argv[i][0] == '%') {
            int id = atoi(argv[i] + 1);
            Job *j = job_find_id(id);
            if (!j) { fprintf(stderr, "mysh: kill: %%%d: no such job\n", id); return 1; }
            killpg(j->pgid, sig);
        } else {
            pid_t pid = (pid_t)atoi(argv[i]);
            if (kill(pid, sig) < 0)
                fprintf(stderr, "mysh: kill: %s: %s\n", argv[i], strerror(errno));
        }
    }
    return 0;
}
/* ── Builtin dispatch ─────────────────────────────────────────────────────── */

static int try_builtin(AstNode *node, int *out_status)
{
    if (!node || node->kind != NODE_CMD || node->argc == 0) return 0;
    if (node->redirs) return 0;

    char **argv = node->argv;
    int count = 0;
    while (argv[count]) count++;
    char **exp = malloc(sizeof(char *) * ((size_t)count + 1));
    if (!exp) return 0;
    for (int j = 0; j < count; j++) exp[j] = argv[j];
    exp[count] = NULL;
    expand_argv(&exp);
    argv = exp;

    int status = 0;
    int matched = 1;

    if      (strcmp(argv[0], "exit") == 0) handle_builtin_exit(argv);
    else if (strcmp(argv[0], "cd")   == 0) status = handle_builtin_cd(argv);
    else if (strcmp(argv[0], "jobs") == 0) status = handle_builtin_jobs();
    else if (strcmp(argv[0], "fg")   == 0) status = handle_builtin_fg(argv);
    else if (strcmp(argv[0], "bg")   == 0) status = handle_builtin_bg(argv);
    else if (strcmp(argv[0], "kill") == 0) status = handle_builtin_kill(argv);
    else matched = 0;

    free(exp);
    *out_status = status;
    return matched;
}

/* ── Main REPL ────────────────────────────────────────────────────────────── */

int shell_run(int argc, char **argv)
{
    (void)argc;

    g_shell.argv0       = argv[0];
    g_shell.interactive = isatty(STDIN_FILENO);
    g_shell.pgid        = getpgrp();

    setup_signals();

    while (1) {
        /* Report completed background jobs */
        if (g_got_sigchld) {
            g_got_sigchld = 0;
            job_check_and_report();
        }

        if (g_got_sigint) {
            g_got_sigint = 0;
            fputc('\n', stderr);
        }

        print_prompt();

        char *line = read_line(stdin);
        if (!line) {
            if (g_shell.interactive) fputc('\n', stderr);
            break;
        }

        /* Skip blank/comment lines */
        size_t i = 0;
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') i++;
        if (line[i] == '\0' || line[i] == '#') { free(line); continue; }

        Lexer   *lexer = lexer_new(line);
        if (!lexer) { free(line); continue; }

        AstNode *ast = parse(lexer);
        lexer_free(lexer);
        free(line);

        if (!ast) continue;

        int status;
        if (!try_builtin(ast, &status))
            status = exec_node(ast);

        g_shell.last_status = status;
        ast_free(ast);
    }

    return g_shell.last_status;
}
