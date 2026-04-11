#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <glob.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "expand.h"
#include "mysh.h"

/* ── Dynamic string buffer ────────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Buf;

static int buf_init(Buf *b)
{
    b->buf = malloc(64);
    if (!b->buf) return -1;
    b->buf[0] = '\0';
    b->len = 0;
    b->cap = 64;
    return 0;
}

static int buf_push(Buf *b, char c)
{
    if (b->len + 1 >= b->cap) {
        size_t nc = b->cap * 2;
        char  *t  = realloc(b->buf, nc);
        if (!t) return -1;
        b->buf = t;
        b->cap = nc;
    }
    b->buf[b->len++] = c;
    b->buf[b->len]   = '\0';
    return 0;
}

static int buf_append(Buf *b, const char *s)
{
    while (*s)
        if (buf_push(b, *s++) < 0) return -1;
    return 0;
}

static char *buf_take(Buf *b)
{
    char *s = realloc(b->buf, b->len + 1);
    if (!s) s = b->buf;
    b->buf = NULL;
    b->len = b->cap = 0;
    return s;
}

static void buf_free(Buf *b) { free(b->buf); b->buf = NULL; }

/* ── Word list ────────────────────────────────────────────────────────────── */

typedef struct {
    char  **words;
    size_t  count;
    size_t  cap;
} WordList;

static int wl_init(WordList *wl)
{
    wl->words = malloc(sizeof(char *) * 8);
    if (!wl->words) return -1;
    wl->count = 0;
    wl->cap   = 8;
    return 0;
}

static int wl_push(WordList *wl, char *s)
{
    if (wl->count + 1 >= wl->cap) {
        size_t  nc = wl->cap * 2;
        char  **t  = realloc(wl->words, sizeof(char *) * nc);
        if (!t) return -1;
        wl->words = t;
        wl->cap   = nc;
    }
    wl->words[wl->count++] = s;
    wl->words[wl->count]   = NULL;
    return 0;
}

/* ── Tilde expansion ──────────────────────────────────────────────────────── */

/*
 * Expand leading ~ or ~username.
 * Returns a heap-allocated string; caller frees.
 * Returns NULL on failure (falls back to original word in caller).
 */
static char *expand_tilde(const char *word)
{
    if (word[0] != '~') return NULL;

    const char *rest = word + 1; /* everything after ~ */

    if (*rest == '\0' || *rest == '/') {
        /* ~/... or just ~ — use $HOME or passwd entry */
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            if (!pw) return NULL;
            home = pw->pw_dir;
        }
        /* Concatenate home + rest */
        size_t hlen = strlen(home);
        size_t rlen = strlen(rest);
        char  *out  = malloc(hlen + rlen + 1);
        if (!out) return NULL;
        memcpy(out, home, hlen);
        memcpy(out + hlen, rest, rlen + 1);
        return out;
    }

    /* ~username/... */
    const char *slash = strchr(rest, '/');
    size_t ulen = slash ? (size_t)(slash - rest) : strlen(rest);
    char  *uname = malloc(ulen + 1);
    if (!uname) return NULL;
    memcpy(uname, rest, ulen);
    uname[ulen] = '\0';

    struct passwd *pw = getpwnam(uname);
    free(uname);
    if (!pw) return NULL;

    size_t hlen = strlen(pw->pw_dir);
    size_t rlen = slash ? strlen(slash) : 0;
    char  *out  = malloc(hlen + rlen + 1);
    if (!out) return NULL;
    memcpy(out, pw->pw_dir, hlen);
    if (slash) memcpy(out + hlen, slash, rlen + 1);
    else out[hlen] = '\0';
    return out;
}

/* ── Command substitution $(...) ─────────────────────────────────────────── */

/*
 * Run `cmd` in a subshell, capture its stdout, strip trailing newlines.
 * Returns heap-allocated string or NULL on error.
 */
static char *run_subst(const char *cmd)
{
    int   pipefd[2];
    if (pipe(pipefd) < 0) { perror("mysh: pipe"); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("mysh: fork");
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* child: redirect stdout → pipe write end, run cmd via sh */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(1);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* parent: read from pipe */
    close(pipefd[1]);

    Buf b;
    if (buf_init(&b) < 0) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    char tmp[256];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0)
        for (ssize_t i = 0; i < n; i++)
            buf_push(&b, tmp[i]);
    close(pipefd[0]);

    int ws;
    while (waitpid(pid, &ws, 0) == -1 && errno == EINTR);

    /* Strip trailing newlines (POSIX) */
    while (b.len > 0 && b.buf[b.len - 1] == '\n')
        b.buf[--b.len] = '\0';

    return buf_take(&b);
}

/* ── Parameter + command substitution scan ───────────────────────────────── */

/*
 * Scan `word`, performing parameter ($VAR ${VAR} $? $$ $#) and
 * command substitution $(...).  Writes result into `out` Buf.
 * `dquote` = 1 means we're inside double quotes (no glob later).
 */
static int expand_params(const char *word, Buf *out, int dquote)
{
    (void)dquote;
    const char *p = word;

    while (*p) {
        if (*p != '$') { buf_push(out, *p++); continue; }
        p++; /* skip $ */

        if (*p == '(') {
            /* Command substitution $(...) */
            p++; /* skip ( */
            int depth = 1;
            const char *start = p;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
                else p++;
            }
            /* p now points one past the closing ) */
            size_t cmdlen = (size_t)(p - start - 1);
            char  *cmd    = malloc(cmdlen + 1);
            if (!cmd) return -1;
            memcpy(cmd, start, cmdlen);
            cmd[cmdlen] = '\0';

            char *result = run_subst(cmd);
            free(cmd);
            if (result) {
                buf_append(out, result);
                free(result);
            }
            continue;
        }

        if (*p == '{') {
            /* ${VAR} or ${VAR:-default} */
            p++;
            const char *start = p;
            while (*p && *p != '}' && *p != ':') p++;
            size_t nlen = (size_t)(p - start);
            char  *name = malloc(nlen + 1);
            if (!name) return -1;
            memcpy(name, start, nlen);
            name[nlen] = '\0';

            char *def = NULL;
            if (*p == ':' && *(p+1) == '-') {
                p += 2; /* skip :- */
                const char *ds = p;
                while (*p && *p != '}') p++;
                size_t dlen = (size_t)(p - ds);
                def = malloc(dlen + 1);
                if (!def) { free(name); return -1; }
                memcpy(def, ds, dlen);
                def[dlen] = '\0';
            }
            if (*p == '}') p++;

            const char *val = getenv(name);
            free(name);
            if (val && *val) buf_append(out, val);
            else if (def)    buf_append(out, def);
            free(def);
            continue;
        }

        /* Special parameters */
        if (*p == '?') {
            p++;
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", g_shell.last_status);
            buf_append(out, tmp);
            continue;
        }
        if (*p == '$') {
            p++;
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", (int)getpid());
            buf_append(out, tmp);
            continue;
        }
        if (*p == '#') {
            p++;
            buf_append(out, "0"); /* argc not tracked yet — Phase 5 */
            continue;
        }

        /* Plain $VAR */
        if (*p == '_' || (*p >= 'A' && *p <= 'Z') ||
                         (*p >= 'a' && *p <= 'z')) {
            const char *start = p;
            while (*p == '_' || (*p >= 'A' && *p <= 'Z') ||
                                 (*p >= 'a' && *p <= 'z') ||
                                 (*p >= '0' && *p <= '9')) p++;
            size_t nlen = (size_t)(p - start);
            char  *name = malloc(nlen + 1);
            if (!name) return -1;
            memcpy(name, start, nlen);
            name[nlen] = '\0';
            const char *val = getenv(name);
            free(name);
            if (val) buf_append(out, val);
            continue;
        }

        /* Bare $ with no recognisable follow — keep literally */
        buf_push(out, '$');
    }
    return 0;
}

/* ── Glob expansion ───────────────────────────────────────────────────────── */

#define MAX_GLOB_RESULTS 256

/*
 * If `word` contains glob metacharacters, expand via glob(3).
 * Appends results to `wl`.  If no match, appends the original word (POSIX).
 * Returns 0 on success, -1 on error.
 */
static int expand_glob(const char *word, WordList *wl)
{
    /* Check for metacharacters */
    int has_meta = 0;
    for (const char *p = word; *p; p++)
        if (*p == '*' || *p == '?' || *p == '[') { has_meta = 1; break; }

    if (!has_meta) {
        char *copy = strdup(word);
        if (!copy) return -1;
        return wl_push(wl, copy);
    }

    glob_t gl;
    int    rc = glob(word, GLOB_NOCHECK | GLOB_TILDE, NULL, &gl);

    if (rc != 0 && rc != GLOB_NOMATCH) {
        /* Glob error — fall back to literal */
        globfree(&gl);
        char *copy = strdup(word);
        if (!copy) return -1;
        return wl_push(wl, copy);
    }

    /* Safety cap */
    size_t limit = gl.gl_pathc < MAX_GLOB_RESULTS
                   ? gl.gl_pathc : MAX_GLOB_RESULTS;

    for (size_t i = 0; i < limit; i++) {
        char *copy = strdup(gl.gl_pathv[i]);
        if (!copy) { globfree(&gl); return -1; }
        if (wl_push(wl, copy) < 0) { free(copy); globfree(&gl); return -1; }
    }

    globfree(&gl);
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

char **expand_word(const char *word, int in_double_quote)
{
    if (!word) return NULL;

    WordList wl;
    if (wl_init(&wl) < 0) return NULL;

    /* 1. Tilde expansion (only outside quotes) */
    char *tilde_expanded = NULL;
    if (!in_double_quote && word[0] == '~') {
        tilde_expanded = expand_tilde(word);
    }
    const char *after_tilde = tilde_expanded ? tilde_expanded : word;

    /* 2+3. Parameter + command substitution */
    Buf param_buf;
    if (buf_init(&param_buf) < 0) {
        free(tilde_expanded);
        expand_free(wl.words);
        return NULL;
    }
    if (expand_params(after_tilde, &param_buf, in_double_quote) < 0) {
        buf_free(&param_buf);
        free(tilde_expanded);
        expand_free(wl.words);
        return NULL;
    }
    free(tilde_expanded);
    char *after_params = buf_take(&param_buf);

    /* 4. Glob (skip inside double quotes) */
    if (in_double_quote) {
        wl_push(&wl, after_params);
    } else {
        if (expand_glob(after_params, &wl) < 0) {
            free(after_params);
            expand_free(wl.words);
            return NULL;
        }
        free(after_params);
    }

    return wl.words;
}

int expand_argv(char ***argv_ptr)
{
    if (!argv_ptr || !*argv_ptr) return 0;

    char **old = *argv_ptr;
    WordList wl;
    if (wl_init(&wl) < 0) return -1;

    for (int i = 0; old[i]; i++) {
        char **expanded = expand_word(old[i], 0);
        if (!expanded) { expand_free(wl.words); return -1; }
        for (int j = 0; expanded[j]; j++) {
            if (wl_push(&wl, expanded[j]) < 0) {
                expand_free(expanded);
                expand_free(wl.words);
                return -1;
            }
            expanded[j] = NULL; /* ownership transferred */
        }
        free(expanded);
    }

    *argv_ptr = wl.words;
    return (int)wl.count;
}

void expand_free(char **words)
{
    if (!words) return;
    for (int i = 0; words[i]; i++) free(words[i]);
    free(words);
}

void expand_free_argv(char **argv)
{
    expand_free(argv);
}