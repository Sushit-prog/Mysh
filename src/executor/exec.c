#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "executor.h"
#include "expand.h"
#include "mysh.h"

static int exec_pipeline(AstNode *node);
static pid_t exec_simple(AstNode *node, int in_fd, int out_fd);

Command *cmd_new(char **words, size_t count)
{
    if (count == 0) return NULL;
    Command *cmd = malloc(sizeof(*cmd));
    if (!cmd) { perror("mysh: cmd_new"); return NULL; }
    cmd->argv = malloc(sizeof(char *) * (count + 1));
    if (!cmd->argv) { free(cmd); return NULL; }
    for (size_t i = 0; i < count; i++) {
        cmd->argv[i] = strdup(words[i]);
        if (!cmd->argv[i]) {
            for (size_t j = 0; j < i; j++) free(cmd->argv[j]);
            free(cmd->argv); free(cmd); return NULL;
        }
    }
    cmd->argv[count] = NULL;
    cmd->argc = (int)count;
    return cmd;
}

void cmd_free(Command *cmd)
{
    if (!cmd) return;
    for (int i = 0; i < cmd->argc; i++) free(cmd->argv[i]);
    free(cmd->argv);
    free(cmd);
}

static int wait_for(pid_t pid)
{
    int ws;
    while (waitpid(pid, &ws, 0) == -1)
        if (errno != EINTR) { perror("mysh: waitpid"); return 1; }
    if (WIFEXITED(ws))   return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws)) return 128 + WTERMSIG(ws);
    return 1;
}

/* ── Heredoc pipe creation ────────────────────────────────────────────────── */

/*
 * Given the heredoc content string, create a pipe, write content to the
 * write end in a child, return the read end fd.
 * Returns -1 on error.
 */
static int heredoc_pipe(const char *content)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("mysh: pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("mysh: fork");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        size_t len = strlen(content);
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(pipefd[1], content + written, len - written);
            if (n < 0) _exit(1);
            written += (size_t)n;
        }
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    return pipefd[0];
}

/* ── Redirection application ──────────────────────────────────────────────── */

static int apply_redirs(Redir *r)
{
    while (r) {
        int fd = -1;
        switch (r->kind) {
            case REDIR_IN:
                fd = open(r->target, O_RDONLY | O_CLOEXEC);
                if (fd < 0) { fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); return -1; }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;
            case REDIR_OUT:
                fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0666);
                if (fd < 0) { fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); return -1; }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;
            case REDIR_APPEND:
                fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
                if (fd < 0) { fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); return -1; }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;
            case REDIR_HEREDOC:
                fd = heredoc_pipe(r->target); /* target holds content after parsing */
                if (fd < 0) return -1;
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;
        }
        r = r->next;
    }
    return 0;
}

/* ── Builtin execution in parent ──────────────────────────────────────────── */

static int exec_builtin(AstNode *node)
{
    if (!node || node->kind != NODE_CMD || node->argc == 0) return -1;

    char **argv = node->argv;
    int count = 0;
    while (argv[count]) count++;
    char **exp = malloc(sizeof(char *) * ((size_t)count + 1));
    if (!exp) return -1;
    for (int j = 0; j < count; j++) exp[j] = argv[j];
    exp[count] = NULL;
    expand_argv(&exp);
    argv = exp;

    int status = -1;

    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argv[1] ? argv[1] : getenv("HOME");
        if (!dir) {
            fprintf(stderr, "mysh: cd: HOME not set\n"); status = 1;
        } else if (chdir(dir) < 0) {
            fprintf(stderr, "mysh: cd: %s: %s\n", dir, strerror(errno)); status = 1;
        } else {
            char *cwd = getcwd(NULL, 0);
            if (cwd) { setenv("PWD", cwd, 1); free(cwd); }
            status = 0;
        }
    }

    free(exp);
    return status;
}

/* ── Simple command execution ─────────────────────────────────────────────── */

static pid_t exec_simple(AstNode *node, int in_fd, int out_fd)
{
    if (!node || node->kind != NODE_CMD || node->argc == 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { perror("mysh: fork"); return -1; }

    if (pid == 0) {
        /* Put child in its own process group */
        setpgid(0, 0);

        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        if (in_fd  >= 0 && dup2(in_fd,  STDIN_FILENO)  < 0) { perror("mysh: dup2"); _exit(1); }
        if (out_fd >= 0 && dup2(out_fd, STDOUT_FILENO) < 0) { perror("mysh: dup2"); _exit(1); }
        if (apply_redirs(node->redirs) < 0) _exit(1);

        char **argv = node->argv;
        if (expand_argv(&argv) < 0) _exit(1);
        if (!argv || !argv[0]) _exit(0);

        execvp(argv[0], argv);
        if (errno == ENOENT) fprintf(stderr, "mysh: %s: command not found\n", argv[0]);
        else                 fprintf(stderr, "mysh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent: put child in its own process group */
    setpgid(pid, pid);

    if (in_fd  >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    return pid;
}

/* ── Pipeline execution ───────────────────────────────────────────────────── */

#define MAX_PIPE_STAGES 64

static int exec_pipeline(AstNode *root)
{
    AstNode *stages[MAX_PIPE_STAGES];
    int n = 0;
    AstNode *cur = root;

    while (cur && cur->kind == NODE_PIPE) {
        if (n >= MAX_PIPE_STAGES - 1) { fprintf(stderr, "mysh: pipeline too long\n"); return 1; }
        stages[n++] = cur->right;
        cur = cur->left;
    }
    stages[n++] = cur;
    for (int i = 0, j = n-1; i < j; i++, j--) {
        AstNode *t = stages[i]; stages[i] = stages[j]; stages[j] = t;
    }

    if (n == 1) {
        pid_t p = exec_simple(stages[0], -1, -1);
        return p > 0 ? wait_for(p) : 1;
    }

    int pipes[MAX_PIPE_STAGES-1][2];
    for (int i = 0; i < n-1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("mysh: pipe");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return 1;
        }
        fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC);
    }

    pid_t pids[MAX_PIPE_STAGES];
    for (int i = 0; i < n; i++) {
        int in_fd  = (i == 0)   ? -1 : pipes[i-1][0];
        int out_fd = (i == n-1) ? -1 : pipes[i][1];
        pids[i] = exec_simple(stages[i], in_fd, out_fd);
    }
    for (int i = 0; i < n-1; i++) { close(pipes[i][0]); close(pipes[i][1]); }

    int last = 0;
    for (int i = 0; i < n; i++) {
        int s = pids[i] > 0 ? wait_for(pids[i]) : 1;
        if (i == n-1) last = s;
    }
    return last;
}

/* ── AST walker ───────────────────────────────────────────────────────────── */

int exec_node(AstNode *node)
{
    if (!node) return 0;

    switch (node->kind) {
        case NODE_CMD: {
            int bs = exec_builtin(node);
            if (bs >= 0) return bs;
            pid_t p = exec_simple(node, -1, -1);
            return p > 0 ? wait_for(p) : 1;
        }
        case NODE_PIPE:
            return exec_pipeline(node);
        case NODE_AND: {
            int s = exec_node(node->left);
            return s == 0 ? exec_node(node->right) : s;
        }
        case NODE_OR: {
            int s = exec_node(node->left);
            return s != 0 ? exec_node(node->right) : s;
        }
        case NODE_SEQ:
            exec_node(node->left);
            return exec_node(node->right);
        case NODE_BACKGROUND: {
            pid_t p = fork();
            if (p < 0) { perror("mysh: fork"); return 1; }
            if (p == 0) {
                setpgid(0, 0);
                signal(SIGINT,  SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                signal(SIGTSTP, SIG_IGN);
                _exit(exec_node(node->child));
            }
            setpgid(p, p);
            Job *j = job_add(p, NULL);
            if (j) fprintf(stderr, "[%d] %d\n", j->id, p);
            else   fprintf(stderr, "[?] %d\n", p);
            return 0;
        }
        case NODE_SUBSHELL: {
            pid_t p = fork();
            if (p < 0) { perror("mysh: fork"); return 1; }
            if (p == 0) {
                if (apply_redirs(node->redirs) < 0) _exit(1);
                _exit(exec_node(node->child));
            }
            return wait_for(p);
        }
    }
    return 0;
}

int exec_argv(char **argv)
{
    if (!argv || !argv[0]) return 0;
    AstNode fake = { .kind = NODE_CMD, .argv = argv, .argc = 0, .redirs = NULL };
    while (argv[fake.argc]) fake.argc++;
    pid_t p = exec_simple(&fake, -1, -1);
    return p > 0 ? wait_for(p) : 1;
}

int exec_command(Command *cmd)
{
    if (!cmd) return 0;
    return exec_argv(cmd->argv);
}
