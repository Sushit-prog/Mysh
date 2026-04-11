#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "mysh.h"

Job *job_add(pid_t pgid, const char *cmdline)
{
    if (g_shell.job_count >= MYSH_MAX_JOBS) {
        fprintf(stderr, "mysh: job table full\n");
        return NULL;
    }
    /* Find a free slot */
    for (int i = 0; i < MYSH_MAX_JOBS; i++) {
        if (g_shell.jobs[i].id == 0) {
            g_shell.jobs[i].id      = i + 1;
            g_shell.jobs[i].pgid    = pgid;
            g_shell.jobs[i].state   = JOB_RUNNING;
            g_shell.jobs[i].status  = 0;
            g_shell.jobs[i].cmdline = cmdline ? strdup(cmdline) : NULL;
            g_shell.job_count++;
            return &g_shell.jobs[i];
        }
    }
    return NULL;
}

void job_remove(int job_id)
{
    for (int i = 0; i < MYSH_MAX_JOBS; i++) {
        if (g_shell.jobs[i].id == job_id) {
            free(g_shell.jobs[i].cmdline);
            memset(&g_shell.jobs[i], 0, sizeof(Job));
            g_shell.job_count--;
            return;
        }
    }
}

Job *job_find_pgid(pid_t pgid)
{
    for (int i = 0; i < MYSH_MAX_JOBS; i++)
        if (g_shell.jobs[i].id != 0 && g_shell.jobs[i].pgid == pgid)
            return &g_shell.jobs[i];
    return NULL;
}

Job *job_find_id(int id)
{
    for (int i = 0; i < MYSH_MAX_JOBS; i++)
        if (g_shell.jobs[i].id == id)
            return &g_shell.jobs[i];
    return NULL;
}

/*
 * Called before each prompt. Non-blocking waitpid loop over all jobs.
 * Reports completed/stopped jobs and removes them from the table.
 */
void job_check_and_report(void)
{
    for (int i = 0; i < MYSH_MAX_JOBS; i++) {
        Job *j = &g_shell.jobs[i];
        if (j->id == 0) continue;

        int ws;
        pid_t r = waitpid(-j->pgid, &ws, WNOHANG | WUNTRACED);
        if (r == 0) continue; /* still running */

        if (r < 0) {
            /* Process already reaped or doesn't exist */
            if (j->state == JOB_DONE) {
                fprintf(stderr, "[%d] Done\t\t%s\n", j->id,
                        j->cmdline ? j->cmdline : "");
                job_remove(j->id);
            }
            continue;
        }

        if (WIFSTOPPED(ws)) {
            j->state  = JOB_STOPPED;
            j->status = WSTOPSIG(ws);
            fprintf(stderr, "[%d] Stopped\t\t%s\n", j->id,
                    j->cmdline ? j->cmdline : "");
        } else {
            j->state  = JOB_DONE;
            j->status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
            fprintf(stderr, "[%d] Done\t\t%s\n", j->id,
                    j->cmdline ? j->cmdline : "");
            job_remove(j->id);
        }
    }
}
