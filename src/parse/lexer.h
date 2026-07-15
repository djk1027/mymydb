#ifndef MYMYDB_LEXER_H
#define MYMYDB_LEXER_H

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,   /* identifiers and keywords (case-insensitive match in parser) */
    TOK_NUMBER,  /* integer literal */
    TOK_STRING,  /* 'single-quoted' text literal */

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_STAR,
    TOK_SEMICOLON,

    TOK_EQ,      /* =  */
    TOK_NE,      /* != or <> */
    TOK_LT,      /* <  */
    TOK_LE,      /* <= */
    TOK_GT,      /* >  */
    TOK_GE,      /* >= */

    TOK_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    const char *start; /* points into the source buffer */
    int len;
} Token;

typedef struct {
    const char *src;
    const char *cur;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);

#endif /* MYMYDB_LEXER_H */
