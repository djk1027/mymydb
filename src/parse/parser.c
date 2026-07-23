#include "parser.h"
#include "arena.h"
#include "lexer.h"

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

typedef struct {
    Lexer lx;
    Token cur;
    Arena *arena; /* backs every AST allocation; freed wholesale on error */
    char *errbuf;
    int errcap;
    jmp_buf jmp;  /* jumped to on parse error */
} Parser;

/* ---- error handling ----------------------------------------------------- */

static void fail(Parser *p, const char *fmt, ...) {
    if (p->errbuf && p->errcap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->errbuf, p->errcap, fmt, ap);
        va_end(ap);
    }
    longjmp(p->jmp, 1);
}

/* ---- token helpers ------------------------------------------------------ */

static void advance(Parser *p) { p->cur = lexer_next(&p->lx); }

static bool tok_is_kw(const Token *t, const char *kw) {
    return t->type == TOK_IDENT &&
           (int)strlen(kw) == t->len &&
           strncasecmp(t->start, kw, t->len) == 0;
}

static bool accept(Parser *p, TokenType type) {
    if (p->cur.type == type) { advance(p); return true; }
    return false;
}

static bool accept_kw(Parser *p, const char *kw) {
    if (tok_is_kw(&p->cur, kw)) { advance(p); return true; }
    return false;
}

static void expect(Parser *p, TokenType type, const char *what) {
    if (p->cur.type != type)
        fail(p, "expected %s near '%.*s'", what,
             p->cur.len ? p->cur.len : 1,
             p->cur.len ? p->cur.start : "");
    advance(p);
}

static void expect_kw(Parser *p, const char *kw) {
    if (!accept_kw(p, kw))
        fail(p, "expected '%s' near '%.*s'", kw,
             p->cur.len ? p->cur.len : 1,
             p->cur.len ? p->cur.start : "");
}

/* Copies the current IDENT into a fixed buffer and advances. */
static void take_ident(Parser *p, char *dst, int cap, const char *what) {
    if (p->cur.type != TOK_IDENT)
        fail(p, "expected %s near '%.*s'", what,
             p->cur.len ? p->cur.len : 1,
             p->cur.len ? p->cur.start : "");
    int n = p->cur.len < cap - 1 ? p->cur.len : cap - 1;
    memcpy(dst, p->cur.start, n);
    dst[n] = '\0';
    advance(p);
}

/* Duplicates the current IDENT into the arena and advances. */
static char *dup_ident(Parser *p, const char *what) {
    if (p->cur.type != TOK_IDENT)
        fail(p, "expected %s near '%.*s'", what,
             p->cur.len ? p->cur.len : 1,
             p->cur.len ? p->cur.start : "");
    char *s = arena_strndup(p->arena, p->cur.start, p->cur.len);
    advance(p);
    return s;
}

/* ---- literal / value parsing ------------------------------------------- */

static Value parse_literal(Parser *p) {
    if (p->cur.type == TOK_NUMBER) {
        char buf[32];
        if (p->cur.len >= (int)sizeof(buf))
            fail(p, "integer literal out of range: '%.*s'",
                 p->cur.len, p->cur.start);
        memcpy(buf, p->cur.start, p->cur.len);
        buf[p->cur.len] = '\0';

        errno = 0;
        char *end = NULL;
        long long v = strtoll(buf, &end, 10);
        if (errno == ERANGE || end == buf || *end != '\0')
            fail(p, "integer literal out of range: '%s'", buf);
        advance(p);
        return value_int(v);
    }
    if (p->cur.type == TOK_STRING) {
        /* Decode into the arena, collapsing doubled '' escapes to one quote. */
        char *s = arena_alloc(p->arena, p->cur.len + 1);
        int w = 0;
        for (int r = 0; r < p->cur.len; r++) {
            s[w++] = p->cur.start[r];
            if (p->cur.start[r] == '\'' && r + 1 < p->cur.len &&
                p->cur.start[r + 1] == '\'')
                r++;
        }
        s[w] = '\0';
        advance(p);
        Value v = {.type = TYPE_TEXT, .is_null = false};
        v.as.s = s; /* arena-owned; not freed via value_free */
        return v;
    }
    fail(p, "expected a literal near '%.*s'",
         p->cur.len ? p->cur.len : 1, p->cur.len ? p->cur.start : "");
    return value_null(TYPE_INT); /* unreachable */
}

/* ---- WHERE expression (recursive descent) ------------------------------- */
/* Grammar:  or_expr := and_expr (OR and_expr)*
 *           and_expr := cmp_expr (AND cmp_expr)*
 *           cmp_expr := column OP literal  |  '(' or_expr ')'      */

static Expr *parse_or(Parser *p);

static Expr *new_binary(Parser *p, OpKind op, Expr *l, Expr *r) {
    Expr *e = arena_calloc(p->arena, sizeof(Expr));
    e->kind = EXPR_BINARY;
    e->as.binary.op = op;
    e->as.binary.left = l;
    e->as.binary.right = r;
    return e;
}

static bool comparison_op(Parser *p, OpKind *out) {
    switch (p->cur.type) {
        case TOK_EQ: *out = OP_EQ; break;
        case TOK_NE: *out = OP_NE; break;
        case TOK_LT: *out = OP_LT; break;
        case TOK_LE: *out = OP_LE; break;
        case TOK_GT: *out = OP_GT; break;
        case TOK_GE: *out = OP_GE; break;
        default: return false;
    }
    advance(p);
    return true;
}

static Expr *parse_cmp(Parser *p) {
    if (accept(p, TOK_LPAREN)) {
        Expr *e = parse_or(p);
        expect(p, TOK_RPAREN, "')'");
        return e;
    }

    /* column OP literal */
    Expr *col = arena_calloc(p->arena, sizeof(Expr));
    col->kind = EXPR_COLUMN;
    col->as.column = dup_ident(p, "column name");

    OpKind op;
    if (!comparison_op(p, &op))
        fail(p, "expected comparison operator near '%.*s'",
             p->cur.len ? p->cur.len : 1, p->cur.len ? p->cur.start : "");

    Expr *lit = arena_calloc(p->arena, sizeof(Expr));
    lit->kind = EXPR_LITERAL;
    lit->as.literal = parse_literal(p);

    return new_binary(p, op, col, lit);
}

static Expr *parse_and(Parser *p) {
    Expr *left = parse_cmp(p);
    while (accept_kw(p, "AND"))
        left = new_binary(p, OP_AND, left, parse_cmp(p));
    return left;
}

static Expr *parse_or(Parser *p) {
    Expr *left = parse_and(p);
    while (accept_kw(p, "OR"))
        left = new_binary(p, OP_OR, left, parse_and(p));
    return left;
}

/* ---- statements --------------------------------------------------------- */

static ColType parse_coltype(Parser *p) {
    if (accept_kw(p, "INT") || accept_kw(p, "INTEGER"))
        return TYPE_INT;
    if (accept_kw(p, "TEXT") || accept_kw(p, "VARCHAR") || accept_kw(p, "STRING"))
        return TYPE_TEXT;
    fail(p, "expected column type (INT|TEXT) near '%.*s'",
         p->cur.len ? p->cur.len : 1, p->cur.len ? p->cur.start : "");
    return TYPE_INT; /* unreachable */
}

static Stmt *parse_create(Parser *p) {
    if (accept_kw(p, "DATABASE")) {
        Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
        s->type = STMT_CREATE_DATABASE;
        take_ident(p, s->as.db.name, MAX_NAME, "database name");
        return s;
    }

    expect_kw(p, "TABLE");

    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_CREATE;
    take_ident(p, s->as.create.table, MAX_NAME, "table name");

    expect(p, TOK_LPAREN, "'('");

    int cap = 4, n = 0;
    Column *cols = arena_alloc(p->arena, cap * sizeof(Column));
    do {
        if (n == cap) {
            int newcap = cap * 2;
            cols = arena_realloc(p->arena, cols, cap * sizeof(Column),
                                 newcap * sizeof(Column));
            cap = newcap;
        }
        Column c = {0};
        take_ident(p, c.name, MAX_NAME, "column name");
        c.type = parse_coltype(p);
        cols[n++] = c;
    } while (accept(p, TOK_COMMA));

    expect(p, TOK_RPAREN, "')'");

    s->as.create.cols = cols;
    s->as.create.ncols = n;
    return s;
}

static Stmt *parse_insert(Parser *p) {
    expect_kw(p, "INTO");

    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_INSERT;
    take_ident(p, s->as.insert.table, MAX_NAME, "table name");

    expect_kw(p, "VALUES");
    expect(p, TOK_LPAREN, "'('");

    int cap = 4, n = 0;
    Value *vals = arena_alloc(p->arena, cap * sizeof(Value));
    do {
        if (n == cap) {
            int newcap = cap * 2;
            vals = arena_realloc(p->arena, vals, cap * sizeof(Value),
                                 newcap * sizeof(Value));
            cap = newcap;
        }
        vals[n++] = parse_literal(p);
    } while (accept(p, TOK_COMMA));

    expect(p, TOK_RPAREN, "')'");

    s->as.insert.values = vals;
    s->as.insert.nvalues = n;
    return s;
}

static bool agg_func_from(const char *name, AggFunc *out) {
    if (strcasecmp(name, "COUNT") == 0) { *out = AGG_COUNT; return true; }
    if (strcasecmp(name, "SUM") == 0)   { *out = AGG_SUM;   return true; }
    if (strcasecmp(name, "AVG") == 0)   { *out = AGG_AVG;   return true; }
    if (strcasecmp(name, "MIN") == 0)   { *out = AGG_MIN;   return true; }
    if (strcasecmp(name, "MAX") == 0)   { *out = AGG_MAX;   return true; }
    return false;
}

/* Parses the select list: either "*", plain columns, or aggregate calls.
 * Mixing plain columns and aggregates is rejected. */
static void parse_select_list(Parser *p, SelectStmt *sel) {
    if (accept(p, TOK_STAR)) {
        sel->select_all = true;
        return;
    }

    int ccap = 4, ncols = 0;
    char **cols = arena_alloc(p->arena, ccap * sizeof(char *));
    int acap = 4, naggs = 0;
    AggCall *aggs = arena_alloc(p->arena, acap * sizeof(AggCall));

    do {
        if (p->cur.type != TOK_IDENT)
            fail(p, "expected column or function near '%.*s'",
                 p->cur.len ? p->cur.len : 1, p->cur.len ? p->cur.start : "");
        char *name = arena_strndup(p->arena, p->cur.start, p->cur.len);
        advance(p);

        if (accept(p, TOK_LPAREN)) {
            AggFunc func;
            if (!agg_func_from(name, &func))
                fail(p, "unknown function '%s'", name);
            AggCall call = {.func = func};
            if (accept(p, TOK_STAR))
                call.star = true;
            else
                call.column = dup_ident(p, "column name");
            expect(p, TOK_RPAREN, "')'");

            if (naggs == acap) {
                int newcap = acap * 2;
                aggs = arena_realloc(p->arena, aggs, acap * sizeof(AggCall),
                                     newcap * sizeof(AggCall));
                acap = newcap;
            }
            aggs[naggs++] = call;
        } else {
            if (ncols == ccap) {
                int newcap = ccap * 2;
                cols = arena_realloc(p->arena, cols, ccap * sizeof(char *),
                                     newcap * sizeof(char *));
                ccap = newcap;
            }
            cols[ncols++] = name;
        }
    } while (accept(p, TOK_COMMA));

    if (naggs > 0 && ncols > 0)
        fail(p, "cannot mix aggregate functions with plain columns");

    if (naggs > 0) {
        sel->is_agg = true;
        sel->aggs = aggs;
        sel->naggs = naggs;
    } else {
        sel->cols = cols;
        sel->ncols = ncols;
    }
}

static void parse_order_by(Parser *p, SelectStmt *sel) {
    expect_kw(p, "BY");
    int cap = 4, n = 0;
    OrderKey *keys = arena_alloc(p->arena, cap * sizeof(OrderKey));
    do {
        if (n == cap) {
            int newcap = cap * 2;
            keys = arena_realloc(p->arena, keys, cap * sizeof(OrderKey),
                                 newcap * sizeof(OrderKey));
            cap = newcap;
        }
        OrderKey k = {.desc = false};
        k.column = dup_ident(p, "column name");
        if (accept_kw(p, "DESC")) k.desc = true;
        else (void)accept_kw(p, "ASC");
        keys[n++] = k;
    } while (accept(p, TOK_COMMA));
    sel->order = keys;
    sel->norder = n;
}

static Stmt *parse_select(Parser *p) {
    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_SELECT;

    parse_select_list(p, &s->as.select);

    expect_kw(p, "FROM");
    take_ident(p, s->as.select.table, MAX_NAME, "table name");

    if (accept_kw(p, "WHERE"))
        s->as.select.where = parse_or(p);

    if (accept_kw(p, "ORDER"))
        parse_order_by(p, &s->as.select);

    return s;
}

static Stmt *parse_delete(Parser *p) {
    expect_kw(p, "FROM");

    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_DELETE;
    take_ident(p, s->as.del.table, MAX_NAME, "table name");

    if (accept_kw(p, "WHERE"))
        s->as.del.where = parse_or(p);

    return s;
}

static Stmt *parse_use(Parser *p) {
    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_USE;
    take_ident(p, s->as.db.name, MAX_NAME, "database name");
    return s;
}

static Stmt *parse_show(Parser *p) {
    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_SHOW;
    if (accept_kw(p, "DATABASES")) {
        s->as.show.kind = SHOW_DATABASES;
    } else if (accept_kw(p, "TABLES")) {
        s->as.show.kind = SHOW_TABLES;
    } else if (accept_kw(p, "PARAMETERS")) {
        s->as.show.kind = SHOW_PARAMETERS;
    } else if (accept_kw(p, "GLOBAL")) {
        expect_kw(p, "PARAMETERS");
        s->as.show.kind = SHOW_GLOBAL_PARAMETERS;
    } else if (accept_kw(p, "CREATE")) {
        expect_kw(p, "TABLE");
        s->as.show.kind = SHOW_CREATE_TABLE;
        take_ident(p, s->as.show.name, MAX_NAME, "table name");
    } else {
        fail(p, "expected DATABASES|TABLES|PARAMETERS|CREATE TABLE after SHOW "
                "near '%.*s'",
             p->cur.len ? p->cur.len : 1, p->cur.len ? p->cur.start : "");
    }
    return s;
}

static Stmt *parse_set(Parser *p) {
    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = STMT_SET;
    if (accept_kw(p, "GLOBAL")) s->as.set.global = true;
    take_ident(p, s->as.set.name, MAX_NAME, "parameter name");
    expect(p, TOK_EQ, "'='");

    Value v = parse_literal(p);
    if (v.type == TYPE_INT) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", (long long)v.as.i);
        s->as.set.value = arena_strndup(p->arena, buf, strlen(buf));
    } else {
        s->as.set.value = v.as.s; /* arena-owned string from parse_literal */
    }
    return s;
}

static Stmt *parse_bare(Parser *p, StmtType type) {
    Stmt *s = arena_calloc(p->arena, sizeof(Stmt));
    s->type = type;
    return s;
}

/* ---- entry point -------------------------------------------------------- */

Stmt *parse_statement(const char *sql, char *errbuf, int errcap) {
    Parser p = {0};
    p.errbuf = errbuf;
    p.errcap = errcap;
    p.arena = arena_new();
    lexer_init(&p.lx, sql);

    if (setjmp(p.jmp)) {
        arena_destroy(p.arena); /* releases any half-built AST */
        return NULL;
    }

    advance(&p);
    if (p.cur.type == TOK_EOF)
        fail(&p, "empty statement");

    Stmt *s;
    if (accept_kw(&p, "CREATE"))      s = parse_create(&p);
    else if (accept_kw(&p, "INSERT")) s = parse_insert(&p);
    else if (accept_kw(&p, "SELECT")) s = parse_select(&p);
    else if (accept_kw(&p, "DELETE")) s = parse_delete(&p);
    else if (accept_kw(&p, "USE"))    s = parse_use(&p);
    else if (accept_kw(&p, "SHOW"))   s = parse_show(&p);
    else if (accept_kw(&p, "SET"))    s = parse_set(&p);
    else if (accept_kw(&p, "HELP"))   s = parse_bare(&p, STMT_HELP);
    else if (accept_kw(&p, "EXIT") ||
             accept_kw(&p, "QUIT"))   s = parse_bare(&p, STMT_EXIT);
    else { fail(&p, "unknown statement near '%.*s'",
                p.cur.len ? p.cur.len : 1, p.cur.len ? p.cur.start : "");
           return NULL; /* unreachable; silences -Wmaybe-uninitialized */ }

    s->arena = p.arena; /* hand the arena's ownership to the finished AST */

    accept(&p, TOK_SEMICOLON);
    if (p.cur.type != TOK_EOF)
        fail(&p, "unexpected trailing input near '%.*s'",
             p.cur.len ? p.cur.len : 1, p.cur.len ? p.cur.start : "");

    return s;
}
