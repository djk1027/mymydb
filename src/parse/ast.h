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

/* ---- Aggregates & ordering (v1.2) --------------------------------------- */

typedef enum {
    AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX,
} AggFunc;

typedef struct {
    AggFunc func;
    bool star;       /* COUNT(*) */
    char *column;    /* argument column name (arena); NULL when star */
} AggCall;

typedef struct {
    char *column;    /* ORDER BY key column (arena) */
    bool desc;       /* true for DESC, false for ASC */
} OrderKey;

/* ---- Statements --------------------------------------------------------- */

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT,
    STMT_DELETE,
    STMT_CREATE_DATABASE,
    STMT_USE,
    STMT_SHOW,    /* SHOW DATABASES|TABLES|PARAMETERS|GLOBAL PARAMETERS|CREATE TABLE t */
    STMT_SET,     /* SET [GLOBAL] name = value */
    STMT_HELP,    /* HELP */
    STMT_EXIT,    /* EXIT / QUIT (handled by the REPL) */
    STMT_CHECKPOINT, /* CHECKPOINT: flush all databases to disk */
    STMT_UPDATE,       /* UPDATE t SET col = val, ... WHERE ... */
    STMT_CREATE_INDEX, /* CREATE INDEX name ON t (col, ...) */
    STMT_DROP_INDEX,   /* DROP INDEX name */
} StmtType;

typedef enum {
    SHOW_DATABASES,
    SHOW_TABLES,
    SHOW_PARAMETERS,
    SHOW_GLOBAL_PARAMETERS,
    SHOW_CREATE_TABLE,
} ShowKind;

typedef struct {
    ShowKind kind;
    char name[MAX_NAME];   /* table name for SHOW CREATE TABLE */
} ShowStmt;

typedef struct {
    bool global;           /* SET GLOBAL vs SET */
    char name[MAX_NAME];
    char *value;           /* arena-owned string form of the value */
} SetStmt;

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
    char **cols;          /* projected column names; NULL when select_all/is_agg */
    int ncols;

    bool is_agg;          /* select list is aggregate functions */
    AggCall *aggs;        /* aggregate calls; valid when is_agg */
    int naggs;

    Expr *where;          /* nullable */

    OrderKey *order;      /* ORDER BY keys; NULL when none */
    int norder;
} SelectStmt;

typedef struct {
    char table[MAX_NAME];
    Expr *where;          /* nullable: DELETE FROM t with no WHERE clears all */
} DeleteStmt;

typedef struct {
    char table[MAX_NAME];
    char **cols;          /* columns being assigned (arena) */
    Value *vals;          /* assigned literals, parallel to cols (arena) */
    int nset;
    Expr *where;          /* nullable */
} UpdateStmt;

typedef struct {
    char name[MAX_NAME];
    char table[MAX_NAME];
    char **cols;          /* indexed column names (arena) */
    int ncols;
} CreateIndexStmt;

/* CREATE DATABASE <name> / USE <name> */
typedef struct {
    char name[MAX_NAME];
} DbStmt;

typedef struct {
    StmtType type;
    Arena *arena;   /* backing storage for this statement's whole AST */
    union {
        CreateStmt create;
        InsertStmt insert;
        SelectStmt select;
        DeleteStmt del;
        DbStmt db;
        ShowStmt show;
        SetStmt set;
        UpdateStmt update;
        CreateIndexStmt create_index;
    } as;
} Stmt;

/* Frees the statement and everything it references (its arena). */
void stmt_free(Stmt *s);

#endif /* MYMYDB_AST_H */
