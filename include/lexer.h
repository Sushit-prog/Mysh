#ifndef MYSH_LEXER_H
#define MYSH_LEXER_H

#include <stddef.h>

/* ── Token types ──────────────────────────────────────────────────────────── */
typedef enum {
    TOK_WORD,           /* bare word or quoted string             */
    TOK_PIPE,           /* |                                      */
    TOK_AND,            /* &&                                     */
    TOK_OR,             /* ||                                     */
    TOK_BACKGROUND,     /* &                                      */
    TOK_REDIR_IN,       /* <                                      */
    TOK_REDIR_OUT,      /* >                                      */
    TOK_REDIR_APPEND,   /* >>                                     */
    TOK_REDIR_HEREDOC,  /* <<                                     */
    TOK_LPAREN,         /* (                                      */
    TOK_RPAREN,         /* )                                      */
    TOK_SEMICOLON,      /* ;                                      */
    TOK_NEWLINE,        /* \n                                     */
    TOK_EOF,            /* end of input                           */
    TOK_ERROR           /* lexer error                            */
} TokenType;

typedef struct {
    TokenType  type;
    char      *value;   /* heap-allocated; NULL for non-WORD tokens */
    size_t     line;    /* 1-based line number                      */
    size_t     col;     /* 1-based column                           */
} Token;

/* ── Lexer handle (opaque) ────────────────────────────────────────────────── */
typedef struct Lexer Lexer;

Lexer      *lexer_new(const char *input);  /* input not owned by lexer     */
Token       lexer_next(Lexer *l);          /* consume next token            */
Token       lexer_peek(Lexer *l);          /* peek without consuming        */
void        lexer_free(Lexer *l);
void        token_free(Token *t);
const char *token_type_name(TokenType t);

#endif /* MYSH_LEXER_H */