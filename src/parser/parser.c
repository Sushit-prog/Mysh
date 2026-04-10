#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "parser.h"
#include "lexer.h"

/*
 * Grammar (simplified, left-recursion eliminated):
 *
 *   list        := pipeline ( ('&&' | '||') pipeline )*
 *   pipeline    := command ( '|' command )*
 *   command     := word+ redir*
 *               |  '(' list ')' redir*
 *   redir       := ('<' | '>' | '>>' | '<<') WORD
 *
 * A complete input line is a list optionally followed by '&', ';', '\n', EOF.
 */

/* ── Node constructors ────────────────────────────────────────────────────── */

static AstNode *node_new(NodeKind kind)
{
    AstNode *n = calloc(1, sizeof(*n));
    if (!n) { perror("mysh: parser malloc"); return NULL; }
    n->kind = kind;
    return n;
}

static Redir *redir_new(RedirKind kind, int fd, char *target)
{
    Redir *r = calloc(1, sizeof(*r));
    if (!r) { perror("mysh: redir malloc"); return NULL; }
    r->kind   = kind;
    r->fd     = fd;
    r->target = target; /* takes ownership */
    return r;
}

/* ── AST cleanup ──────────────────────────────────────────────────────────── */

static void redir_free(Redir *r)
{
    while (r) {
        Redir *next = r->next;
        free(r->target);
        free(r);
        r = next;
    }
}

void ast_free(AstNode *n)
{
    if (!n) return;
    switch (n->kind) {
        case NODE_CMD:
            if (n->argv) {
                for (int i = 0; i < n->argc; i++) free(n->argv[i]);
                free(n->argv);
            }
            redir_free(n->redirs);
            break;
        case NODE_PIPE:
        case NODE_AND:
        case NODE_OR:
        case NODE_SEQ:
            ast_free(n->left);
            ast_free(n->right);
            break;
        case NODE_BACKGROUND:
        case NODE_SUBSHELL:
            ast_free(n->child);
            redir_free(n->redirs);
            break;
    }
    free(n);
}

/* ── Token helpers ────────────────────────────────────────────────────────── */

/* Consume the current token if it matches type; return 1 on success */
static int accept(Lexer *l, TokenType type, Token *out)
{
    Token t = lexer_peek(l);
    if (t.type != type) return 0;
    *out = lexer_next(l);
    return 1;
}

/* Return 1 if the next token is a command terminator */
static int is_terminator(TokenType t)
{
    return t == TOK_NEWLINE || t == TOK_SEMICOLON ||
           t == TOK_EOF     || t == TOK_RPAREN;
}

/* ── Redirection parsing ──────────────────────────────────────────────────── */

/*
 * Attempt to parse one redirection.
 * Returns a heap-allocated Redir* or NULL if the next token isn't a redir op.
 * On error (missing filename), prints a message and returns NULL.
 */
static Redir *parse_redir(Lexer *l)
{
    Token op = lexer_peek(l);
    RedirKind kind;
    int fd;

    switch (op.type) {
        case TOK_REDIR_IN:      kind = REDIR_IN;     fd = 0; break;
        case TOK_REDIR_OUT:     kind = REDIR_OUT;    fd = 1; break;
        case TOK_REDIR_APPEND:  kind = REDIR_APPEND; fd = 1; break;
        case TOK_REDIR_HEREDOC: kind = REDIR_HEREDOC; fd = 0; break;
        default: return NULL;
    }
    lexer_next(l); /* consume the operator */

    Token target = lexer_next(l);
    if (target.type != TOK_WORD) {
        fprintf(stderr, "mysh:%zu:%zu: expected filename after redirect\n",
                op.line, op.col);
        token_free(&target);
        return NULL;
    }

    return redir_new(kind, fd, target.value); /* target.value ownership transferred */
}

/* ── Command parsing ──────────────────────────────────────────────────────── */

/*
 * Parse a simple command: one or more WORDs plus any number of redirections.
 * Redirections may appear before, between, or after words (POSIX).
 */
static AstNode *parse_command(Lexer *l)
{
    /* Handle subshell: ( list ) */
    if (lexer_peek(l).type == TOK_LPAREN) {
        lexer_next(l); /* consume '(' */

        AstNode *inner = parse(l); /* recurse for inner list */

        Token rp;
        if (!accept(l, TOK_RPAREN, &rp)) {
            fprintf(stderr, "mysh: expected ')'\n");
            ast_free(inner);
            return NULL;
        }

        AstNode *n = node_new(NODE_SUBSHELL);
        if (!n) { ast_free(inner); return NULL; }
        n->child = inner;

        /* Redirections after ')' apply to the subshell */
        Redir  *head = NULL, **tail = &head;
        Redir  *r;
        while ((r = parse_redir(l)) != NULL) { *tail = r; tail = &r->next; }
        n->redirs = head;

        return n;
    }

    /* Collect argv words and redirections */
    size_t  cap  = 8, argc = 0;
    char  **argv = malloc(sizeof(char *) * cap);
    if (!argv) { perror("mysh: parser malloc"); return NULL; }

    Redir *head = NULL, **tail = &head;

    while (1) {
        /* Try a redirection first */
        Redir *r = parse_redir(l);
        if (r) { *tail = r; tail = &r->next; continue; }

        /* Then a word */
        Token t = lexer_peek(l);
        if (t.type != TOK_WORD) break;
        lexer_next(l);

        if (argc + 1 >= cap) {
            cap *= 2;
            char **tmp = realloc(argv, sizeof(char *) * cap);
            if (!tmp) {
                perror("mysh: parser realloc");
                free(t.value);
                break;
            }
            argv = tmp;
        }
        argv[argc++] = t.value; /* transfer ownership */
    }

    if (argc == 0 && head == NULL) {
        free(argv);
        return NULL; /* empty — not an error, just nothing here */
    }

    argv[argc] = NULL;

    AstNode *n = node_new(NODE_CMD);
    if (!n) {
        for (size_t i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        redir_free(head);
        return NULL;
    }
    n->argv   = argv;
    n->argc   = (int)argc;
    n->redirs = head;
    return n;
}

/* ── Pipeline parsing ─────────────────────────────────────────────────────── */

static AstNode *parse_pipeline(Lexer *l)
{
    AstNode *left = parse_command(l);
    if (!left) return NULL;

    while (lexer_peek(l).type == TOK_PIPE) {
        lexer_next(l); /* consume '|' */

        AstNode *right = parse_command(l);
        if (!right) {
            fprintf(stderr, "mysh: expected command after '|'\n");
            ast_free(left);
            return NULL;
        }

        AstNode *pipe = node_new(NODE_PIPE);
        if (!pipe) { ast_free(left); ast_free(right); return NULL; }
        pipe->left  = left;
        pipe->right = right;
        left = pipe; /* left-associate */
    }

    return left;
}

/* ── List parsing (&&, ||, ;, &) ─────────────────────────────────────────── */

/*
 * parse() is the top-level entry point.
 * It handles && / || (equal precedence, left-associative per POSIX),
 * then ; sequencing, then & backgrounding.
 */
AstNode *parse(Lexer *l)
{
    /* Skip leading newlines/semicolons */
    while (1) {
        TokenType tt = lexer_peek(l).type;
        if (tt == TOK_NEWLINE || tt == TOK_SEMICOLON)
            lexer_next(l);
        else
            break;
    }

    AstNode *left = parse_pipeline(l);
    if (!left) return NULL;

    while (1) {
        TokenType tt = lexer_peek(l).type;

        if (tt == TOK_AND || tt == TOK_OR) {
            NodeKind kind = (tt == TOK_AND) ? NODE_AND : NODE_OR;
            lexer_next(l); /* consume && or || */

            /* Skip newlines after operator */
            while (lexer_peek(l).type == TOK_NEWLINE) lexer_next(l);

            AstNode *right = parse_pipeline(l);
            if (!right) {
                fprintf(stderr, "mysh: expected command after '%s'\n",
                        tt == TOK_AND ? "&&" : "||");
                ast_free(left);
                return NULL;
            }
            AstNode *n = node_new(kind);
            if (!n) { ast_free(left); ast_free(right); return NULL; }
            n->left  = left;
            n->right = right;
            left = n;

        } else if (tt == TOK_SEMICOLON || tt == TOK_NEWLINE) {
            lexer_next(l); /* consume ; or \n */

            /* Peek ahead — if another command follows, make a SEQ node */
            TokenType next = lexer_peek(l).type;
            if (is_terminator(next)) break; /* trailing ; is fine */

            AstNode *right = parse(l);
            if (!right) break;

            AstNode *seq = node_new(NODE_SEQ);
            if (!seq) { ast_free(left); ast_free(right); return NULL; }
            seq->left  = left;
            seq->right = right;
            return seq; /* right-recursive already consumed the rest */

        } else if (tt == TOK_BACKGROUND) {
            lexer_next(l); /* consume & */
            AstNode *bg = node_new(NODE_BACKGROUND);
            if (!bg) { ast_free(left); return NULL; }
            bg->child = left;
            left = bg;
            /* After &, there may be another command */
            TokenType next = lexer_peek(l).type;
            if (is_terminator(next)) break;
            AstNode *rest = parse(l);
            if (!rest) break;
            AstNode *seq = node_new(NODE_SEQ);
            if (!seq) { ast_free(left); ast_free(rest); return NULL; }
            seq->left  = left;
            seq->right = rest;
            return seq;

        } else {
            break;
        }
    }

    return left;
}