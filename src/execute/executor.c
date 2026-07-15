#include "executor.h"
#include "optimizer.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ---- CREATE TABLE ------------------------------------------------------- */

static bool exec_create(Database *db, const CreateStmt *c, FILE *out,
                        char *errbuf, int errcap) {
    /* reject duplicate column names */
    for (int i = 0; i < c->ncols; i++)
        for (int j = i + 1; j < c->ncols; j++)
            if (strcmp(c->cols[i].name, c->cols[j].name) == 0) {
                snprintf(errbuf, errcap, "duplicate column: %s", c->cols[i].name);
                return false;
            }

    if (!db_create_table(db, c->table, c->cols, c->ncols)) {
        snprintf(errbuf, errcap, "table already exists: %s", c->table);
        return false;
    }
    fprintf(out, "OK: table '%s' created (%d column%s)\n",
            c->table, c->ncols, c->ncols == 1 ? "" : "s");
    return true;
}

/* ---- INSERT ------------------------------------------------------------- */

/* Coerces a literal to the column's declared type, or fails. */
static bool coerce(const Value *lit, ColType want, Value *out,
                   char *errbuf, int errcap) {
    if (lit->is_null) { *out = value_null(want); return true; }
    if (lit->type == want) { *out = value_copy(lit); return true; }
    snprintf(errbuf, errcap, "type mismatch: expected %s, got %s",
             coltype_name(want), coltype_name(lit->type));
    return false;
}

static bool exec_insert(Database *db, const InsertStmt *ins, FILE *out,
                        char *errbuf, int errcap) {
    Table *t = db_find_table(db, ins->table);
    if (!t) {
        snprintf(errbuf, errcap, "no such table: %s", ins->table);
        return false;
    }
    if (ins->nvalues != t->ncols) {
        snprintf(errbuf, errcap,
                 "table '%s' has %d columns but %d values were supplied",
                 t->name, t->ncols, ins->nvalues);
        return false;
    }

    Value *cells = malloc(t->ncols * sizeof(Value));
    for (int i = 0; i < t->ncols; i++) {
        if (!coerce(&ins->values[i], t->cols[i].type, &cells[i],
                    errbuf, errcap)) {
            for (int j = 0; j < i; j++) value_free(&cells[j]);
            free(cells);
            return false;
        }
    }
    table_append_row(t, cells);
    fprintf(out, "OK: 1 row inserted into '%s'\n", t->name);
    return true;
}

/* ---- WHERE evaluation --------------------------------------------------- */

static int cmp_values(const Value *a, const Value *b) {
    if (a->type == TYPE_INT)
        return (a->as.i > b->as.i) - (a->as.i < b->as.i);
    return strcmp(a->as.s ? a->as.s : "", b->as.s ? b->as.s : "");
}

static bool apply_cmp(OpKind op, int c) {
    switch (op) {
        case OP_EQ: return c == 0;
        case OP_NE: return c != 0;
        case OP_LT: return c < 0;
        case OP_LE: return c <= 0;
        case OP_GT: return c > 0;
        case OP_GE: return c >= 0;
        default:    return false;
    }
}

/* Evaluate a predicate against one row. NULLs and type mismatches -> false. */
static bool eval_pred(const Expr *e, const Table *t, const Row *row) {
    if (!e) return true;
    if (e->kind != EXPR_BINARY) return false; /* shouldn't happen at top level */

    OpKind op = e->as.binary.op;
    if (op == OP_AND)
        return eval_pred(e->as.binary.left, t, row) &&
               eval_pred(e->as.binary.right, t, row);
    if (op == OP_OR)
        return eval_pred(e->as.binary.left, t, row) ||
               eval_pred(e->as.binary.right, t, row);

    /* comparison: left = column, right = literal (per grammar) */
    const Expr *col = e->as.binary.left;
    const Expr *lit = e->as.binary.right;
    int idx = table_col_index(t, col->as.column);
    if (idx < 0) return false;

    const Value *cell = &row->cells[idx];
    const Value *val = &lit->as.literal;
    if (cell->is_null || val->is_null) return false;
    if (cell->type != val->type) return false;

    return apply_cmp(op, cmp_values(cell, val));
}

/* ---- SELECT ------------------------------------------------------------- */

static char *value_to_str(const Value *v) {
    if (v->is_null) return strdup("NULL");
    if (v->type == TYPE_INT) {
        char buf[32];
        snprintf(buf, sizeof buf, "%" PRId64, v->as.i);
        return strdup(buf);
    }
    return strdup(v->as.s ? v->as.s : "");
}

static bool exec_select(Database *db, const SelectStmt *sel, FILE *out,
                        char *errbuf, int errcap) {
    Plan plan;
    if (!optimizer_plan_select(db, sel, &plan, errbuf, errcap))
        return false;

    Table *t = plan.table;

    /* collect indices of matching rows */
    int *hits = malloc((t->nrows ? t->nrows : 1) * sizeof(int));
    int nhits = 0;
    for (int r = 0; r < t->nrows; r++)
        if (eval_pred(plan.filter, t, &t->rows[r]))
            hits[nhits++] = r;

    /* render every cell to a string so we can align columns */
    int ncol = plan.nproj;
    int *width = calloc(ncol, sizeof(int));
    char **header = malloc(ncol * sizeof(char *));
    for (int c = 0; c < ncol; c++) {
        header[c] = strdup(t->cols[plan.proj[c]].name);
        width[c] = (int)strlen(header[c]);
    }

    char ***cells = malloc((nhits ? nhits : 1) * sizeof(char **));
    for (int i = 0; i < nhits; i++) {
        cells[i] = malloc(ncol * sizeof(char *));
        Row *row = &t->rows[hits[i]];
        for (int c = 0; c < ncol; c++) {
            cells[i][c] = value_to_str(&row->cells[plan.proj[c]]);
            int w = (int)strlen(cells[i][c]);
            if (w > width[c]) width[c] = w;
        }
    }

    /* header */
    for (int c = 0; c < ncol; c++)
        fprintf(out, "%s%-*s", c ? " | " : "", width[c], header[c]);
    fprintf(out, "\n");
    for (int c = 0; c < ncol; c++) {
        if (c) fprintf(out, "-+-");
        for (int k = 0; k < width[c]; k++) fputc('-', out);
    }
    fprintf(out, "\n");

    /* rows */
    for (int i = 0; i < nhits; i++) {
        for (int c = 0; c < ncol; c++)
            fprintf(out, "%s%-*s", c ? " | " : "", width[c], cells[i][c]);
        fprintf(out, "\n");
    }
    fprintf(out, "(%d row%s)\n", nhits, nhits == 1 ? "" : "s");

    /* cleanup */
    for (int i = 0; i < nhits; i++) {
        for (int c = 0; c < ncol; c++) free(cells[i][c]);
        free(cells[i]);
    }
    free(cells);
    for (int c = 0; c < ncol; c++) free(header[c]);
    free(header);
    free(width);
    free(hits);
    plan_free(&plan);
    return true;
}

/* ---- dispatch ----------------------------------------------------------- */

bool execute(Database *db, const Stmt *stmt, FILE *out,
             char *errbuf, int errcap) {
    switch (stmt->type) {
        case STMT_CREATE: return exec_create(db, &stmt->as.create, out, errbuf, errcap);
        case STMT_INSERT: return exec_insert(db, &stmt->as.insert, out, errbuf, errcap);
        case STMT_SELECT: return exec_select(db, &stmt->as.select, out, errbuf, errcap);
    }
    snprintf(errbuf, errcap, "unknown statement type");
    return false;
}
