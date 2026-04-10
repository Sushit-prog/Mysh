#ifndef MYSH_PARSER_H
#define MYSH_PARSER_H

#include <stddef.h>
#include "lexer.h"

/* ── AST node kinds ───────────────────────────────────────────────────────── */
typedef enum {
    NODE_CMD,        /* leaf: simple command with argv + redirects  */
    NODE_PIPE,       /* binary: left | right                        */
    NODE_AND,        /* binary: left && right                       */
    NODE_OR,         /* binary: left || right                       */
    NODE_SEQ,        /* binary: left ; right                        */
    NODE_BACKGROUND, /* unary:  child &                             */
    NODE_SUBSHELL    /* unary:  ( child )                           */
} NodeKind;

/* ── Redirection ──────────────────────────────────────────────────────────── */
typedef enum {
    REDIR_IN,       /* < file    */
    REDIR_OUT,      /* > file    */
    REDIR_APPEND,   /* >> file   */
    REDIR_HEREDOC   /* << delim  */
} RedirKind;

typedef struct Redir {
    RedirKind   kind;
    int         fd;       /* which fd to redirect (default: 0 or 1) */
    char       *target;   /* filename or heredoc delimiter           */
    struct Redir *next;
} Redir;

/* ── AST node ─────────────────────────────────────────────────────────────── */
typedef struct AstNode {
    NodeKind kind;

    /* NODE_CMD */
    char    **argv;       /* NULL-terminated                        */
    int       argc;
    Redir    *redirs;     /* linked list of redirections            */

    /* NODE_PIPE, NODE_AND, NODE_OR, NODE_SEQ */
    struct AstNode *left;
    struct AstNode *right;

    /* NODE_BACKGROUND, NODE_SUBSHELL */
    struct AstNode *child;
} AstNode;

/* ── Parser API ───────────────────────────────────────────────────────────── */

/*
 * Parse a complete command line from the lexer.
 * Returns the root AstNode*, or NULL on empty input / parse error.
 * Caller must free with ast_free().
 */
AstNode *parse(Lexer *l);

/* Recursively free an AST */
void ast_free(AstNode *node);

#endif /* MYSH_PARSER_H */