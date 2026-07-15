#include "lexer.h"

#include <ctype.h>

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->cur = src;
}

static Token make(TokenType type, const char *start, int len) {
    Token t = {.type = type, .start = start, .len = len};
    return t;
}

Token lexer_next(Lexer *lx) {
    const char *p = lx->cur;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') {
        lx->cur = p;
        return make(TOK_EOF, p, 0);
    }

    const char *start = p;

    /* Identifier / keyword: [A-Za-z_][A-Za-z0-9_]* */
    if (isalpha((unsigned char)*p) || *p == '_') {
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        lx->cur = p;
        return make(TOK_IDENT, start, (int)(p - start));
    }

    /* Number: digits (optionally signed handled by parser). */
    if (isdigit((unsigned char)*p)) {
        while (isdigit((unsigned char)*p)) p++;
        lx->cur = p;
        return make(TOK_NUMBER, start, (int)(p - start));
    }

    /* String literal: '...'  ('' is an escaped quote). */
    if (*p == '\'') {
        p++; /* opening quote */
        const char *content = p;
        while (*p) {
            if (*p == '\'') {
                if (p[1] == '\'') { p += 2; continue; } /* escaped quote */
                break;
            }
            p++;
        }
        if (*p != '\'') { /* unterminated */
            lx->cur = p;
            return make(TOK_ERROR, start, (int)(p - start));
        }
        int len = (int)(p - content);
        p++; /* closing quote */
        lx->cur = p;
        return make(TOK_STRING, content, len);
    }

    /* Operators and punctuation. */
    switch (*p) {
        case '(': lx->cur = p + 1; return make(TOK_LPAREN, start, 1);
        case ')': lx->cur = p + 1; return make(TOK_RPAREN, start, 1);
        case ',': lx->cur = p + 1; return make(TOK_COMMA, start, 1);
        case '*': lx->cur = p + 1; return make(TOK_STAR, start, 1);
        case ';': lx->cur = p + 1; return make(TOK_SEMICOLON, start, 1);
        case '=': lx->cur = p + 1; return make(TOK_EQ, start, 1);
        case '<':
            if (p[1] == '=') { lx->cur = p + 2; return make(TOK_LE, start, 2); }
            if (p[1] == '>') { lx->cur = p + 2; return make(TOK_NE, start, 2); }
            lx->cur = p + 1; return make(TOK_LT, start, 1);
        case '>':
            if (p[1] == '=') { lx->cur = p + 2; return make(TOK_GE, start, 2); }
            lx->cur = p + 1; return make(TOK_GT, start, 1);
        case '!':
            if (p[1] == '=') { lx->cur = p + 2; return make(TOK_NE, start, 2); }
            break;
    }

    lx->cur = p + 1;
    return make(TOK_ERROR, start, 1);
}
