#ifndef MYSH_H
#define MYSH_H

#include <sys/types.h>

/* ── Version ──────────────────────────────────────────────────────────────── */
#define MYSH_VERSION "0.2.0"
#define MYSH_NAME    "mysh"

/* ── Exit status codes ────────────────────────────────────────────────────── */
#define MYSH_OK      0
#define MYSH_ERR     1
#define MYSH_EXIT    2
#define MYSH_MISUSE  2

/* ── REPL limits ──────────────────────────────────────────────────────────── */
#define MYSH_LINE_MAX  4096
#define MYSH_ARG_MAX   1024
#define MYSH_MAX_JOBS  64

/* ── Job states ───────────────────────────────────────────────────────────── */
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

/* ── Job table entry ──────────────────────────────────────────────────────── */
typedef struct {
    int      id;        /* job number (1-based)    */
    pid_t    pgid;      /* process group id        */
    JobState state;
    int      status;    /* last exit status        */
    char    *cmdline;   /* original command string */
} Job;

/* ── Shell global state ───────────────────────────────────────────────────── */
typedef struct {
    int   last_status;
    int   interactive;
    char *argv0;
    pid_t pgid;              /* shell's own process group */

    /* Job table */
    Job   jobs[MYSH_MAX_JOBS];
    int   job_count;
} ShellState;

/* ── Alias table ──────────────────────────────────────────────────────────── */
#define MYSH_MAX_ALIASES 128

typedef struct {
    char *name;
    char *value;
} Alias;

extern ShellState g_shell;
extern Alias g_aliases[MYSH_MAX_ALIASES];
extern int   g_alias_count;
const char *alias_lookup(const char *name);

/* ── Job table API (used by executor and builtins) ───────────────────────── */
Job *job_add(pid_t pgid, const char *cmdline);
void job_remove(int job_id);
Job *job_find_pgid(pid_t pgid);
Job *job_find_id(int id);
void job_check_and_report(void);  /* call before each prompt */

/* ── Entry points ─────────────────────────────────────────────────────────── */
int shell_run(int argc, char **argv);
char *read_heredoc(const char *delim);

#endif /* MYSH_H */
