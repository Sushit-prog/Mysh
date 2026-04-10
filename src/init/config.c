#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "mysh.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"

/* ── Global shell state ───────────────────────────────────────────────────── */
ShellState g_shell = {
    .last_status = 0,
    .interactive = 0,
    .argv0       = NULL,
};

/* ── Signal handling ──────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_got_sigint = 0;

static void sigint_handler(int sig) { (void)sig; g_got_sigint = 1; }

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
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

/* ── Built-in: exit ───────────────────────────────────────────────────────── */

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

/* ── Built-in: cd ─────────────────────────────────────────────────────────── */

static int handle_builtin_cd(char **argv)
{
    const char *dir = argv[1] ? argv[1] : getenv("HOME");
    if (!dir) { fprintf(stderr, "mysh: cd: HOME not set\n"); return 1; }
    if (chdir(dir) < 0) {
        fprintf(stderr, "mysh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

/* ── Built-in dispatch ────────────────────────────────────────────────────── */
/*
 * Returns 1 if the node was a built-in (status written to *out_status).
 * Returns 0 if the node should be handed to exec_node().
 *
 * Only NODE_CMD with zero redirections runs as a built-in in the parent
 * process.  A built-in inside a pipeline runs in a child (exec_node handles
 * that transparently in Phase 5 when we add all built-ins).
 */
static int try_builtin(AstNode *node, int *out_status)
{
    if (!node || node->kind != NODE_CMD || node->argc == 0) return 0;
    if (node->redirs) return 0; /* let exec_node handle redirected builtins */

    char **argv = node->argv;

    if (strcmp(argv[0], "exit") == 0) {
        handle_builtin_exit(argv);
        /* never returns */
    }
    if (strcmp(argv[0], "cd") == 0) {
        *out_status = handle_builtin_cd(argv);
        return 1;
    }

    return 0; /* not a built-in */
}

/* ── Main REPL ────────────────────────────────────────────────────────────── */

int shell_run(int argc, char **argv)
{
    (void)argc;

    g_shell.argv0       = argv[0];
    g_shell.interactive = isatty(STDIN_FILENO);

    setup_signals();

    while (1) {

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

        /* Skip blank / comment lines cheaply */
        size_t i = 0;
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') i++;
        if (line[i] == '\0' || line[i] == '#') { free(line); continue; }

        /* ── Lex + Parse ──────────────────────────────────────────────── */
        Lexer   *lexer = lexer_new(line);
        if (!lexer) { free(line); continue; }

        AstNode *ast = parse(lexer);

        lexer_free(lexer);
        free(line);

        if (!ast) continue;

        /* ── Execute ──────────────────────────────────────────────────── */
        int status;
        if (!try_builtin(ast, &status))
            status = exec_node(ast);

        g_shell.last_status = status;
        ast_free(ast);
    }

    return g_shell.last_status;
}