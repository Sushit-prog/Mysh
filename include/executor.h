#ifndef MYSH_EXECUTOR_H
#define MYSH_EXECUTOR_H

#include <stddef.h>
#include "parser.h"

/* ── Legacy simple-command struct (Phase 1 compat) ───────────────────────── */
typedef struct {
    char  **argv;
    int     argc;
} Command;

Command *cmd_new(char **words, size_t count);
void     cmd_free(Command *cmd);

/* ── Core execution functions ─────────────────────────────────────────────── */

/*
 * Execute a parsed AST node.
 * This is the primary entry point for Phase 2+.
 * Returns the exit status of the last foreground command.
 */
int exec_node(AstNode *node);

/*
 * Execute a NULL-terminated argv array directly (used internally
 * and for the Phase-1 built-in exit fallback).
 */
int exec_argv(char **argv);

/* Execute a Command struct (thin wrapper around exec_argv) */
int exec_command(Command *cmd);

#endif /* MYSH_EXECUTOR_H */