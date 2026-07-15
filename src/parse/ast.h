#ifndef MYMYDB_AST_H
#define MYMYDB_AST_H

#include "arena.h"
#include "storage.h" /* Column, MAX_NAME */
#include "value.h"

/*
 * The whole AST for one statement is allocated from a single Arena (see the
 * `arena` field on Stmt). All pointers below therefore point into that arena
 * and are released together by stmt_free; they are not individually owned.
 */

/* ---- WHERE expression tree ---------------------------------------------- */

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, /* comparisons */
    OP_AND, OP_OR,                            /* logical */
} OpKind;

typedef enum {
    EXPR_COLUMN,   /* a column reference */
    EXPR_LITERAL,  /* a constant value */
    EXPR_BINARY,   /* left OP right */
} ExprKind;

typedef struct Expr {
    ExprKind kind;
    union {
        char *column;   /* EXPR_COLUMN */
        Value literal;  /* EXPR_LITERAL */
        struct {        /* EXPR_BINARY */
            OpKind op;
            struct Expr *left;
            struct Expr *right;
        } binary;
    } as;
} Expr;

/* ---- Statements --------------------------------------------------------- */

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT,
} StmtType;

typedef struct {
    char table[MAX_NAME];
    Column *cols;
    int ncols;
} CreateStmt;

typedef struct {
    char table[MAX_NAME];
    Value *values;   /* positional, in table column order */
    int nvalues;
} InsertStmt;

typedef struct {
    char table[MAX_NAME];
    bool select_all;      /* SELECT * */
    char **cols;          /* projected column names (owned); NULL when select_all */
    int ncols;
    Expr *where;          /* nullable */
} SelectStmt;

typedef struct {
    StmtType type;
    Arena *arena;   /* backing storage for this statement's whole AST */
    union {
        CreateStmt create;
        InsertStmt insert;
        SelectStmt select;
    } as;
} Stmt;

/* Frees the statement and everything it references (its arena). */
void stmt_free(Stmt *s);

#endif /* MYMYDB_AST_H */
