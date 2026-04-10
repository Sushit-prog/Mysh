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
#include "mysh.h"

/* ── Forward declarations ─────────────────────────────────────────────────── */
static int exec_pipeline(AstNode *node);
static int exec_simple(AstNode *node, int in_fd, int out_fd);

/* ── Command struct helpers (Phase 1 compat) ─────────────────────────────── */

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
    cmd->argc        = (int)count;
    return cmd;
}

void cmd_free(Command *cmd)
{
    if (!cmd) return;
    for (int i = 0; i < cmd->argc; i++) free(cmd->argv[i]);
    free(cmd->argv);
    free(cmd);
}

/* ── Wait helper ──────────────────────────────────────────────────────────── */

static int wait_for(pid_t pid)
{
    int ws;
    while (waitpid(pid, &ws, 0) == -1)
        if (errno != EINTR) { perror("mysh: waitpid"); return 1; }
    if (WIFEXITED(ws))   return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws)) return 128 + WTERMSIG(ws);
    return 1;
}

/* ── Redirection application ──────────────────────────────────────────────── */

/*
 * Apply a linked list of Redir structs in the current process.
 * Called inside the child after fork().
 * Returns 0 on success, -1 on error (child should _exit).
 */
static int apply_redirs(Redir *r)
{
    while (r) {
        int fd = -1;
        switch (r->kind) {
            case REDIR_IN:
                fd = open(r->target, O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
                    return -1;
                }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;

            case REDIR_OUT:
                fd = open(r->target,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                          O_NOFOLLOW,   /* don't follow symlinks — security */
                          0666);
                if (fd < 0) {
                    fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
                    return -1;
                }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;

            case REDIR_APPEND:
                fd = open(r->target,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                          0666);
                if (fd < 0) {
                    fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
                    return -1;
                }
                if (dup2(fd, r->fd) < 0) { perror("mysh: dup2"); close(fd); return -1; }
                close(fd);
                break;

            case REDIR_HEREDOC:
                /*
                 * Phase 4 will implement full heredoc with a pipe+writer child.
                 * For now we emit a clear warning rather than silently ignoring.
                 */
                fprintf(stderr, "mysh: heredoc not yet implemented\n");
                return -1;
        }
        r = r->next;
    }
    return 0;
}

/* ── Simple command execution ─────────────────────────────────────────────── */

/*
 * Fork + exec a NODE_CMD.
 * in_fd / out_fd: if >= 0, dup2 them onto stdin/stdout before exec
 * (used by the pipeline builder).  The caller is responsible for closing
 * these fds in the parent after the fork.
 */
static int exec_simple(AstNode *node, int in_fd, int out_fd)
{
    if (!node || node->kind != NODE_CMD || node->argc == 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { perror("mysh: fork"); return 1; }

    if (pid == 0) {
        /* ── Child ── */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        /* Wire pipeline fds */
        if (in_fd  >= 0 && dup2(in_fd,  STDIN_FILENO)  < 0) { perror("mysh: dup2"); _exit(1); }
        if (out_fd >= 0 && dup2(out_fd, STDOUT_FILENO) < 0) { perror("mysh: dup2"); _exit(1); }

        /* Apply redirections (override pipeline fds if both present) */
        if (apply_redirs(node->redirs) < 0) _exit(1);

        execvp(node->argv[0], node->argv);
        if (errno == ENOENT)
            fprintf(stderr, "mysh: %s: command not found\n", node->argv[0]);
        else
            fprintf(stderr, "mysh: %s: %s\n", node->argv[0], strerror(errno));
        _exit(127);
    }

    /* ── Parent ── */
    /* Close the fd ends we passed in — they're now in the child */
    if (in_fd  >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);

    return pid; /* caller decides when to wait */
}

/* ── Pipeline execution ───────────────────────────────────────────────────── */

/*
 * Collect all commands in a left-associative pipeline into an array,
 * create n-1 pipes, fork each stage, then wait for all of them.
 *
 * We build the array first so we know N before allocating pipes.
 */

#define MAX_PIPE_STAGES 64

static int exec_pipeline(AstNode *root)
{
    /* Flatten the left-associative pipe tree into an array */
    AstNode *stages[MAX_PIPE_STAGES];
    int      n = 0;

    AstNode *cur = root;
    while (cur && cur->kind == NODE_PIPE) {
        if (n >= MAX_PIPE_STAGES - 1) {
            fprintf(stderr, "mysh: pipeline too long (max %d)\n", MAX_PIPE_STAGES);
            return 1;
        }
        stages[n++] = cur->right;
        cur = cur->left;
    }
    stages[n++] = cur; /* leftmost command */

    /* Reverse so stages[0] is the first command */
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        AstNode *tmp = stages[i]; stages[i] = stages[j]; stages[j] = tmp;
    }

    if (n == 1) {
        /* No actual pipe — execute directly and wait */
        pid_t pid = (pid_t)exec_simple(stages[0], -1, -1);
        return wait_for(pid);
    }

    /* Create n-1 pipes */
    int pipes[MAX_PIPE_STAGES - 1][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("mysh: pipe");
            /* Close all already-opened pipes */
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return 1;
        }
        /* Set CLOEXEC on both ends — the child's dup2 clears it on the
         * fd it keeps, so only the unneeded ends auto-close on exec.    */
        fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC);
    }

    pid_t pids[MAX_PIPE_STAGES];

    for (int i = 0; i < n; i++) {
        int in_fd  = (i == 0)     ? -1 : pipes[i-1][0];
        int out_fd = (i == n - 1) ? -1 : pipes[i][1];

        pids[i] = (pid_t)exec_simple(stages[i], in_fd, out_fd);
        /*
         * exec_simple closes in_fd and out_fd in the parent after fork.
         * We still need to close the read end of every pipe we no longer
         * need in the parent (exec_simple only closes what it was given).
         */
    }

    /* Close any remaining pipe ends in the parent */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Wait for all children; return the exit status of the last stage */
    int last_status = 0;
    for (int i = 0; i < n; i++) {
        int s = wait_for(pids[i]);
        if (i == n - 1) last_status = s;
    }
    return last_status;
}

/* ── AST walker — main entry point ───────────────────────────────────────── */

int exec_node(AstNode *node)
{
    if (!node) return 0;

    switch (node->kind) {

        case NODE_CMD:
        {
            pid_t pid = (pid_t)exec_simple(node, -1, -1);
            return wait_for(pid);
        }

        case NODE_PIPE:
            return exec_pipeline(node);

        case NODE_AND:
        {
            int s = exec_node(node->left);
            return (s == 0) ? exec_node(node->right) : s;
        }

        case NODE_OR:
        {
            int s = exec_node(node->left);
            return (s != 0) ? exec_node(node->right) : s;
        }

        case NODE_SEQ:
            exec_node(node->left);
            return exec_node(node->right);

        case NODE_BACKGROUND:
        {
            pid_t pid = fork();
            if (pid < 0) { perror("mysh: fork"); return 1; }
            if (pid == 0) {
                /* Child: run the subtree and exit */
                int s = exec_node(node->child);
                _exit(s);
            }
            /* Parent: don't wait — print job info like bash */
            fprintf(stderr, "[bg] %d\n", pid);
            return 0;
        }

        case NODE_SUBSHELL:
        {
            pid_t pid = fork();
            if (pid < 0) { perror("mysh: fork"); return 1; }
            if (pid == 0) {
                if (apply_redirs(node->redirs) < 0) _exit(1);
                int s = exec_node(node->child);
                _exit(s);
            }
            return wait_for(pid);
        }
    }
    return 0;
}

/* ── Legacy helpers ───────────────────────────────────────────────────────── */

int exec_argv(char **argv)
{
    if (!argv || !argv[0]) return 0;
    /* Build a temporary NODE_CMD to reuse exec_simple */
    AstNode fake = { .kind = NODE_CMD, .argv = argv, .argc = 0, .redirs = NULL };
    while (argv[fake.argc]) fake.argc++;
    pid_t pid = (pid_t)exec_simple(&fake, -1, -1);
    return wait_for(pid);
}

int exec_command(Command *cmd)
{
    if (!cmd) return 0;
    return exec_argv(cmd->argv);
}