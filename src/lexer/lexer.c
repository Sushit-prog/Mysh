#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "lexer.h"

/* ── Internal lexer state ─────────────────────────────────────────────────── */
struct Lexer {
    const char *input;
    size_t      pos;
    size_t      line;
    size_t      col;
    int         has_peek;
    Token       peek_tok;
};

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static char l_cur(const Lexer *l)  { return l->input[l->pos]; }

static char l_advance(Lexer *l)
{
    char c = l->input[l->pos];
    if (c != '\0') {
        l->pos++;
        if (c == '\n') { l->line++; l->col = 1; }
        else           { l->col++;              }
    }
    return c;
}

static void l_skip_whitespace(Lexer *l)
{
    while (l_cur(l) == ' ' || l_cur(l) == '\t' || l_cur(l) == '\r')
        l_advance(l);
}

static void l_skip_comment(Lexer *l)
{
    if (l_cur(l) == '#')
        while (l_cur(l) != '\n' && l_cur(l) != '\0')
            l_advance(l);
}

static Token make_simple(TokenType type, size_t line, size_t col)
{
    Token t;
    t.type  = type;
    t.value = NULL;
    t.line  = line;
    t.col   = col;
    return t;
}

static Token make_word(const char *start, size_t len, size_t line, size_t col)
{
    Token t;
    t.type  = TOK_WORD;
    t.line  = line;
    t.col   = col;
    t.value = malloc(len + 1);
    if (!t.value) {
        perror("mysh: lexer malloc");
        return make_simple(TOK_ERROR, line, col);
    }
    memcpy(t.value, start, len);
    t.value[len] = '\0';
    return t;
}

/* ── Quote handling ───────────────────────────────────────────────────────── */

static Token lex_single_quoted(Lexer *l, size_t sl, size_t sc)
{
    l_advance(l); /* consume ' */
    size_t cap = 64, len = 0;
    char  *buf = malloc(cap);
    if (!buf) goto oom;

    while (l_cur(l) != '\'' && l_cur(l) != '\0') {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); goto oom; }
            buf = tmp;
        }
        buf[len++] = l_advance(l);
    }
    if (l_cur(l) == '\0') {
        free(buf);
        fprintf(stderr, "mysh:%zu:%zu: unterminated single quote\n", sl, sc);
        return make_simple(TOK_ERROR, sl, sc);
    }
    l_advance(l); /* consume closing ' */
    buf[len] = '\0';
    Token t = { TOK_WORD, buf, sl, sc };
    return t;
oom:
    perror("mysh: lexer malloc");
    return make_simple(TOK_ERROR, sl, sc);
}

static Token lex_double_quoted(Lexer *l, size_t sl, size_t sc)
{
    l_advance(l); /* consume " */
    size_t cap = 64, len = 0;
    char  *buf = malloc(cap);
    if (!buf) goto oom;

    while (l_cur(l) != '"' && l_cur(l) != '\0') {
        if (len + 2 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); goto oom; }
            buf = tmp;
        }
        if (l_cur(l) == '\\') {
            l_advance(l);
            char esc = l_advance(l);
            switch (esc) {
                case '\\': buf[len++] = '\\'; break;
                case '"':  buf[len++] = '"';  break;
                case '$':  buf[len++] = '$';  break;
                case 'n':  buf[len++] = '\n'; break;
                case 't':  buf[len++] = '\t'; break;
                case '\0': goto unterminated;
                default:   buf[len++] = '\\'; buf[len++] = esc; break;
            }
        } else {
            buf[len++] = l_advance(l);
        }
    }
    if (l_cur(l) == '\0') {
unterminated:
        free(buf);
        fprintf(stderr, "mysh:%zu:%zu: unterminated double quote\n", sl, sc);
        return make_simple(TOK_ERROR, sl, sc);
    }
    l_advance(l); /* consume closing " */
    buf[len] = '\0';
    Token t = { TOK_WORD, buf, sl, sc };
    return t;
oom:
    perror("mysh: lexer malloc");
    return make_simple(TOK_ERROR, sl, sc);
}

/* ── Core lex function ────────────────────────────────────────────────────── */

static Token lex_one(Lexer *l)
{
    l_skip_whitespace(l);
    l_skip_comment(l);

    size_t line = l->line;
    size_t col  = l->col;
    char   c    = l_cur(l);

    if (c == '\0') return make_simple(TOK_EOF,     line, col);
    if (c == '\n') { l_advance(l); return make_simple(TOK_NEWLINE, line, col); }
    if (c == ';')  { l_advance(l); return make_simple(TOK_SEMICOLON, line, col); }
    if (c == '(')  { l_advance(l); return make_simple(TOK_LPAREN,    line, col); }
    if (c == ')')  { l_advance(l); return make_simple(TOK_RPAREN,    line, col); }

    if (c == '|') {
        l_advance(l);
        if (l_cur(l) == '|') { l_advance(l); return make_simple(TOK_OR,   line, col); }
        return make_simple(TOK_PIPE, line, col);
    }
    if (c == '&') {
        l_advance(l);
        if (l_cur(l) == '&') { l_advance(l); return make_simple(TOK_AND,        line, col); }
        return make_simple(TOK_BACKGROUND, line, col);
    }
    if (c == '>') {
        l_advance(l);
        if (l_cur(l) == '>') { l_advance(l); return make_simple(TOK_REDIR_APPEND, line, col); }
        return make_simple(TOK_REDIR_OUT, line, col);
    }
    if (c == '<') {
        l_advance(l);
        if (l_cur(l) == '<') { l_advance(l); return make_simple(TOK_REDIR_HEREDOC, line, col); }
        return make_simple(TOK_REDIR_IN, line, col);
    }

    if (c == '\'') return lex_single_quoted(l, line, col);
    if (c == '"')  return lex_double_quoted(l, line, col);

    /* Bare word */
    size_t start = l->pos;
    while (l_cur(l) != '\0'  &&
           l_cur(l) != ' '   && l_cur(l) != '\t' && l_cur(l) != '\r' &&
           l_cur(l) != '\n'  && l_cur(l) != '|'  && l_cur(l) != '&'  &&
           l_cur(l) != ';'   && l_cur(l) != '<'  && l_cur(l) != '>'  &&
           l_cur(l) != '('   && l_cur(l) != ')'  &&
           l_cur(l) != '\''  && l_cur(l) != '"')
    {
        if (l_cur(l) == '\\') {
            l_advance(l);
            if (l_cur(l) == '\0') break;
        }
        l_advance(l);
    }
    return make_word(l->input + start, l->pos - start, line, col);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

Lexer *lexer_new(const char *input)
{
    Lexer *l = calloc(1, sizeof(*l));
    if (!l) { perror("mysh: lexer_new"); return NULL; }
    l->input = input;
    l->line  = 1;
    l->col   = 1;
    return l;
}

Token lexer_next(Lexer *l)
{
    if (l->has_peek) { l->has_peek = 0; return l->peek_tok; }
    return lex_one(l);
}

Token lexer_peek(Lexer *l)
{
    if (!l->has_peek) { l->peek_tok = lex_one(l); l->has_peek = 1; }
    return l->peek_tok;
}

void lexer_free(Lexer *l)
{
    if (!l) return;
    if (l->has_peek) token_free(&l->peek_tok);
    free(l);
}

void token_free(Token *t)
{
    if (t && t->value) { free(t->value); t->value = NULL; }
}

const char *token_type_name(TokenType t)
{
    switch (t) {
        case TOK_WORD:          return "WORD";
        case TOK_PIPE:          return "PIPE";
        case TOK_AND:           return "AND";
        case TOK_OR:            return "OR";
        case TOK_BACKGROUND:    return "BACKGROUND";
        case TOK_REDIR_IN:      return "REDIR_IN";
        case TOK_REDIR_OUT:     return "REDIR_OUT";
        case TOK_REDIR_APPEND:  return "REDIR_APPEND";
        case TOK_REDIR_HEREDOC: return "REDIR_HEREDOC";
        case TOK_LPAREN:        return "LPAREN";
        case TOK_RPAREN:        return "RPAREN";
        case TOK_SEMICOLON:     return "SEMICOLON";
        case TOK_NEWLINE:       return "NEWLINE";
        case TOK_EOF:           return "EOF";
        case TOK_ERROR:         return "ERROR";
        default:                return "UNKNOWN";
    }
}