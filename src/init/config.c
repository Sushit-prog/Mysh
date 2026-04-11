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
#include "linenoise.h"

static char s_histfile[512] = {0};

/* ── Global state ─────────────────────────────────────────────────────────── */
ShellState g_shell = {
    .last_status = 0,
    .interactive = 0,
    .argv0       = NULL,
    .pgid        = 0,
    .job_count   = 0,
};

Alias g_aliases[MYSH_MAX_ALIASES];
int   g_alias_count = 0;

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
    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
}

/* ── PATH validation ──────────────────────────────────────────────────────── */
static void validate_path(void)
{
    const char *path = getenv("PATH");
    if (!path) return;
    char *copy = strdup(path);
    if (!copy) return;
    char *tok = strtok(copy, ":");
    while (tok) {
        if (tok[0] != '/') {
            fprintf(stderr,
                "mysh: warning: PATH contains relative entry '%s' — "
                "potential hijack risk\n", tok);
        }
        tok = strtok(NULL, ":");
    }
    free(copy);
}

/* ── Prompt ───────────────────────────────────────────────────────────────── */

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
char *read_heredoc(const char *delim)
{
    size_t cap = 256, len = 0;
    char  *buf = malloc(cap);
    if (!buf) return NULL;
    while (1) {
        if (g_shell.interactive) { fprintf(stderr, "> "); fflush(stderr); }
        char *line = read_line(stdin);
        if (!line) break;
        size_t llen = strlen(line);
        if (llen > 0 && line[llen-1] == '\n') line[llen-1] = '\0';
        if (strcmp(line, delim) == 0) { free(line); break; }
        line[llen > 0 ? llen-1 : 0] = '\n';
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

/* ── Alias lookup ─────────────────────────────────────────────────────────── */
const char *alias_lookup(const char *name)
{
    for (int i = 0; i < g_alias_count; i++)
        if (strcmp(g_aliases[i].name, name) == 0)
            return g_aliases[i].value;
    return NULL;
}

/* ── Source a file ────────────────────────────────────────────────────────── */
static int source_file(const char *path);

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

static int handle_builtin_pwd(void)
{
    char *cwd = getcwd(NULL, 0);
    if (!cwd) { perror("mysh: pwd"); return 1; }
    printf("%s\n", cwd);
    free(cwd);
    return 0;
}

static int handle_builtin_export(char **argv)
{
    if (!argv[1]) {
        /* Print all exported vars */
        extern char **environ;
        for (char **e = environ; *e; e++)
            printf("export %s\n", *e);
        return 0;
    }
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            /* export NAME=VALUE */
            char *name = strndup(argv[i], (size_t)(eq - argv[i]));
            if (!name) return 1;
            setenv(name, eq + 1, 1);
            free(name);
        } else {
            /* export NAME — mark existing var for export (it's already in env) */
            const char *val = getenv(argv[i]);
            setenv(argv[i], val ? val : "", 1);
        }
    }
    return 0;
}

static int handle_builtin_unset(char **argv)
{
    for (int i = 1; argv[i]; i++)
        unsetenv(argv[i]);
    return 0;
}

static int handle_builtin_alias(char **argv)
{
    if (!argv[1]) {
        for (int i = 0; i < g_alias_count; i++)
            printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
        return 0;
    }
    /* Rejoin all tokens: alias ll=ls -la → name=ll value="ls -la" */
    char *eq = strchr(argv[1], '=');
    if (!eq) {
        const char *v = alias_lookup(argv[1]);
        if (v) printf("alias %s='%s'\n", argv[1], v);
        else fprintf(stderr, "mysh: alias: %s: not found\n", argv[1]);
        return 0;
    }
    char *name = strndup(argv[1], (size_t)(eq - argv[1]));
    if (!name) return 1;
    /* Build value from eq+1 plus any extra tokens (space-joined) */
    size_t vlen = strlen(eq + 1);
    for (int j = 2; argv[j]; j++) vlen += 1 + strlen(argv[j]);
    char *val = malloc(vlen + 1);
    if (!val) { free(name); return 1; }
    /* Trim leading whitespace from value */
    char *vstart = eq + 1;
    while (*vstart == ' ' || *vstart == '\t') vstart++;
    strcpy(val, vstart);
    for (int j = 2; argv[j]; j++) { strcat(val, " "); strcat(val, argv[j]); }
    /* Update existing or insert */
    for (int j = 0; j < g_alias_count; j++) {
        if (strcmp(g_aliases[j].name, name) == 0) {
            free(g_aliases[j].value);
            g_aliases[j].value = val;
            free(name);
            return 0;
        }
    }
    if (g_alias_count < MYSH_MAX_ALIASES) {
        g_aliases[g_alias_count].name  = name;
        g_aliases[g_alias_count].value = val;
        g_alias_count++;
    } else {
        fprintf(stderr, "mysh: alias table full\n");
        free(name); free(val);
    }
    return 0;
}

static int handle_builtin_unalias(char **argv)
{
    if (!argv[1]) { fprintf(stderr, "mysh: unalias: usage: unalias name\n"); return 1; }
    for (int i = 1; argv[i]; i++) {
        for (int j = 0; j < g_alias_count; j++) {
            if (strcmp(g_aliases[j].name, argv[i]) == 0) {
                free(g_aliases[j].name);
                free(g_aliases[j].value);
                g_aliases[j] = g_aliases[--g_alias_count];
                break;
            }
        }
    }
    return 0;
}

static int handle_builtin_type(char **argv)
{
    if (!argv[1]) { fprintf(stderr, "mysh: type: usage: type name\n"); return 1; }
    for (int i = 1; argv[i]; i++) {
        /* Check alias */
        const char *av = alias_lookup(argv[i]);
        if (av) { printf("%s is aliased to '%s'\n", argv[i], av); continue; }
        /* Check builtins */
        static const char *builtins[] = {
            "cd","exit","export","unset","alias","unalias",
            "source",".","jobs","fg","bg","kill","pwd","type", NULL
        };
        int is_builtin = 0;
        for (int b = 0; builtins[b]; b++)
            if (strcmp(argv[i], builtins[b]) == 0) { is_builtin = 1; break; }
        if (is_builtin) { printf("%s is a shell builtin\n", argv[i]); continue; }
        /* Search PATH */
        const char *path = getenv("PATH");
        if (path) {
            char *pcopy = strdup(path);
            char *tok   = strtok(pcopy, ":");
            int found   = 0;
            while (tok) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", tok, argv[i]);
                if (access(full, X_OK) == 0) {
                    printf("%s is %s\n", argv[i], full);
                    found = 1;
                    break;
                }
                tok = strtok(NULL, ":");
            }
            free(pcopy);
            if (!found) fprintf(stderr, "mysh: type: %s: not found\n", argv[i]);
        }
    }
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
        for (int i = MYSH_MAX_JOBS - 1; i >= 0; i--)
            if (g_shell.jobs[i].id != 0) { id = g_shell.jobs[i].id; break; }
    }
    Job *j = job_find_id(id);
    if (!j) { fprintf(stderr, "mysh: fg: %d: no such job\n", id); return 1; }
    if (g_shell.interactive) tcsetpgrp(STDIN_FILENO, j->pgid);
    if (j->state == JOB_STOPPED) killpg(j->pgid, SIGCONT);
    j->state = JOB_RUNNING;
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
    if (g_shell.interactive) tcsetpgrp(STDIN_FILENO, g_shell.pgid);
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
            int jid = atoi(argv[i] + 1);
            Job *j = job_find_id(jid);
            if (!j) { fprintf(stderr, "mysh: kill: %%%d: no such job\n", jid); return 1; }
            killpg(j->pgid, sig);
        } else {
            pid_t pid = (pid_t)atoi(argv[i]);
            if (kill(pid, sig) < 0)
                fprintf(stderr, "mysh: kill: %s: %s\n", argv[i], strerror(errno));
        }
    }
    return 0;
}

static int handle_builtin_source(char **argv)
{
    if (!argv[1]) { fprintf(stderr, "mysh: source: usage: source file\n"); return 1; }
    return source_file(argv[1]);
}

/* ── Alias expansion ──────────────────────────────────────────────────────── */
/* Expand alias on first word of a NODE_CMD. Returns new heap string or NULL. */
static char *expand_alias_word(const char *word)
{
    const char *val = alias_lookup(word);
    return val ? strdup(val) : NULL;
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

    /* Alias expansion on argv[0] */
    char *alias_val = expand_alias_word(argv[0]);
    if (alias_val) {
        /* Re-lex and re-parse the alias expansion — simple: just swap argv[0] */
        free(alias_val); /* For now just let exec handle it */
        free(exp);
        return 0;
    }

    int status  = 0;
    int matched = 1;

    if      (strcmp(argv[0], "exit")    == 0) handle_builtin_exit(argv);
    else if (strcmp(argv[0], "cd")      == 0) status = handle_builtin_cd(argv);
    else if (strcmp(argv[0], "pwd")     == 0) status = handle_builtin_pwd();
    else if (strcmp(argv[0], "export")  == 0) status = handle_builtin_export(argv);
    else if (strcmp(argv[0], "unset")   == 0) status = handle_builtin_unset(argv);
    else if (strcmp(argv[0], "alias")   == 0) status = handle_builtin_alias(argv);
    else if (strcmp(argv[0], "unalias") == 0) status = handle_builtin_unalias(argv);
    else if (strcmp(argv[0], "type")    == 0) status = handle_builtin_type(argv);
    else if (strcmp(argv[0], "jobs")    == 0) status = handle_builtin_jobs();
    else if (strcmp(argv[0], "fg")      == 0) status = handle_builtin_fg(argv);
    else if (strcmp(argv[0], "bg")      == 0) status = handle_builtin_bg(argv);
    else if (strcmp(argv[0], "kill")    == 0) status = handle_builtin_kill(argv);
    else if (strcmp(argv[0], "source")  == 0) status = handle_builtin_source(argv);
    else if (strcmp(argv[0], ".")       == 0) status = handle_builtin_source(argv);
    else matched = 0;

    free(exp);
    *out_status = status;
    return matched;
}

/* ── Run a line of input ──────────────────────────────────────────────────── */
static int run_line(const char *line)
{
    /* Alias expansion on first word of line */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    /* Extract first word */
    const char *wend = p;
    while (*wend && *wend != ' ' && *wend != '\t' && *wend != '\n') wend++;
    char first[256] = {0};
    size_t wlen = (size_t)(wend - p);
    if (wlen > 0 && wlen < sizeof(first)) {
        memcpy(first, p, wlen);
        first[wlen] = '\0';
        const char *aval = alias_lookup(first);
        if (aval) {
            /* Replace first word with alias value */
            size_t alen = strlen(aval);
            size_t rest = strlen(wend);
            char *expanded = malloc(alen + rest + 2);
            if (expanded) {
                memcpy(expanded, aval, alen);
                memcpy(expanded + alen, wend, rest + 1);
                int r = run_line(expanded);
                free(expanded);
                return r;
            }
        }
    }
    Lexer   *lexer = lexer_new(line);
    if (!lexer) return 1;
    AstNode *ast = parse(lexer);
    lexer_free(lexer);
    if (!ast) return 1;
    int status;
    if (!try_builtin(ast, &status))
        status = exec_node(ast);
    g_shell.last_status = status;
    ast_free(ast);
    return status;
}

/* ── Source a file ────────────────────────────────────────────────────────── */
static int source_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "mysh: source: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char *line;
    while ((line = read_line(fp)) != NULL) {
        size_t i = 0;
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') i++;
        if (line[i] != '\0' && line[i] != '#')
            run_line(line);
        free(line);
    }
    fclose(fp);
    if (s_histfile[0]) linenoiseHistorySave(s_histfile);
    return g_shell.last_status;
}

/* ── Load .myshrc ─────────────────────────────────────────────────────────── */
static void load_rc(void)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.myshrc", home);
    if (access(path, R_OK) == 0)
        source_file(path);
}

/* ── Main REPL ────────────────────────────────────────────────────────────── */
int shell_run(int argc, char **argv)
{
    (void)argc;
    g_shell.argv0       = argv[0];
    g_shell.interactive = isatty(STDIN_FILENO);
    g_shell.pgid        = getpgrp();

    setup_signals();
    validate_path();

    /* History */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(s_histfile, sizeof(s_histfile), "%s/.mysh_history", home);
        linenoiseHistorySetMaxLen(1000);
        linenoiseHistoryLoad(s_histfile);
    }

    load_rc();

    while (1) {
        if (g_got_sigchld) { g_got_sigchld = 0; job_check_and_report(); }
        if (g_got_sigint)  { g_got_sigint  = 0; fputc('\n', stderr); }

        /* Use linenoise for interactive input, read_line for scripts */
        char *line = NULL;
        if (g_shell.interactive) {
            /* Build prompt string */
            char prompt[256];
            const char *ps1 = getenv("PS1");
            if (ps1) {
                snprintf(prompt, sizeof(prompt), "%s", ps1);
            } else {
                char cwd[200];
                if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
                const char *phome = getenv("HOME");
                if (phome && strncmp(cwd, phome, strlen(phome)) == 0)
                    snprintf(prompt, sizeof(prompt), "mysh ~%s> ", cwd + strlen(phome));
                else
                    snprintf(prompt, sizeof(prompt), "mysh %s> ", cwd);
            }
            line = linenoise(prompt);
            if (!line) { fputc('\n', stderr); break; }
            if (line[0] != '\0') linenoiseHistoryAdd(line);
        } else {
            line = read_line(stdin);
            if (!line) break;
        }

        size_t i = 0;
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') i++;
        if (line[i] == '\0' || line[i] == '#') { free(line); continue; }

        run_line(line);
        free(line);
    }
    if (s_histfile[0]) linenoiseHistorySave(s_histfile);
    return g_shell.last_status;
}
