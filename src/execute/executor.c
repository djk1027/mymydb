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

    /* Page storage copies the row in; we always own the transient cells. */
    bool stored = table_append_row(t, cells);
    for (int i = 0; i < t->ncols; i++) value_free(&cells[i]);
    free(cells);

    if (!stored) {
        snprintf(errbuf, errcap, "row too large to fit in a %d-byte page",
                 PAGE_SIZE);
        return false;
    }
    fprintf(out, "OK: 1 row inserted into '%s'\n", t->name);
    return true;
}

/* ---- value comparison / WHERE evaluation -------------------------------- */

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

/* ---- rendering ---------------------------------------------------------- */

static char *value_to_str(const Value *v) {
    if (v->is_null) return strdup("NULL");
    if (v->type == TYPE_INT) {
        char buf[32];
        snprintf(buf, sizeof buf, "%" PRId64, v->as.i);
        return strdup(buf);
    }
    return strdup(v->as.s ? v->as.s : "");
}

/*
 * Print an aligned result grid. header has ncol entries; cells is nrows rows of
 * ncol strings each. Ownership of the arrays stays with the caller.
 */
static void render(FILE *out, int ncol, char **header,
                   int nrows, char ***cells) {
    int *width = calloc(ncol, sizeof(int));
    for (int c = 0; c < ncol; c++) width[c] = (int)strlen(header[c]);
    for (int i = 0; i < nrows; i++)
        for (int c = 0; c < ncol; c++) {
            int w = (int)strlen(cells[i][c]);
            if (w > width[c]) width[c] = w;
        }

    for (int c = 0; c < ncol; c++)
        fprintf(out, "%s%-*s", c ? " | " : "", width[c], header[c]);
    fprintf(out, "\n");
    for (int c = 0; c < ncol; c++) {
        if (c) fprintf(out, "-+-");
        for (int k = 0; k < width[c]; k++) fputc('-', out);
    }
    fprintf(out, "\n");

    for (int i = 0; i < nrows; i++) {
        for (int c = 0; c < ncol; c++)
            fprintf(out, "%s%-*s", c ? " | " : "", width[c], cells[i][c]);
        fprintf(out, "\n");
    }
    fprintf(out, "(%d row%s)\n", nrows, nrows == 1 ? "" : "s");
    free(width);
}

/* ---- SELECT: row path (projection + ORDER BY) --------------------------- */

/* qsort comparator context (single-threaded, so a file-static is fine). */
static const Plan *g_sort_plan;

static int row_cmp(const void *pa, const void *pb) {
    const Row *a = pa, *b = pb;
    for (int k = 0; k < g_sort_plan->norder; k++) {
        int idx = g_sort_plan->order_col[k];
        const Value *va = &a->cells[idx];
        const Value *vb = &b->cells[idx];
        int c;
        if (va->is_null || vb->is_null)
            c = (va->is_null ? 0 : 1) - (vb->is_null ? 0 : 1); /* NULLs first */
        else
            c = cmp_values(va, vb);
        if (c) return g_sort_plan->order_desc[k] ? -c : c;
    }
    return 0;
}

/* Collects every row passing the filter as an owned materialized-row vector. */
static Row *scan_rows(const Plan *plan, const Table *t, int *out_n) {
    int ncols = t->ncols;
    Row *rows = NULL;
    int n = 0, cap = 0;

    Value *scratch = malloc(ncols * sizeof(Value));
    TableCursor cur;
    table_cursor_init(&cur, t);
    while (table_cursor_next(&cur, scratch)) {
        Row tmp = {scratch};
        if (eval_pred(plan->filter, t, &tmp)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                rows = realloc(rows, cap * sizeof(Row));
            }
            rows[n++].cells = scratch;             /* keep this buffer */
            scratch = malloc(ncols * sizeof(Value)); /* fresh one for next row */
        } else {
            for (int c = 0; c < ncols; c++) value_free(&scratch[c]);
        }
    }
    free(scratch); /* last (unused / already-freed) buffer */

    *out_n = n;
    return rows;
}

static void free_rows(Row *rows, int n, int ncols) {
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ncols; c++) value_free(&rows[i].cells[c]);
        free(rows[i].cells);
    }
    free(rows);
}

static void run_rows(const Plan *plan, const Table *t, FILE *out) {
    int nrows;
    Row *rows = scan_rows(plan, t, &nrows);

    if (plan->norder > 0) {
        g_sort_plan = plan;
        qsort(rows, nrows, sizeof(Row), row_cmp);
    }

    int ncol = plan->nproj;
    char **header = malloc(ncol * sizeof(char *));
    for (int c = 0; c < ncol; c++)
        header[c] = strdup(t->cols[plan->proj[c]].name);

    char ***cells = malloc((nrows ? nrows : 1) * sizeof(char **));
    for (int i = 0; i < nrows; i++) {
        cells[i] = malloc(ncol * sizeof(char *));
        for (int c = 0; c < ncol; c++)
            cells[i][c] = value_to_str(&rows[i].cells[plan->proj[c]]);
    }

    render(out, ncol, header, nrows, cells);

    for (int i = 0; i < nrows; i++) {
        for (int c = 0; c < ncol; c++) free(cells[i][c]);
        free(cells[i]);
    }
    free(cells);
    for (int c = 0; c < ncol; c++) free(header[c]);
    free(header);
    free_rows(rows, nrows, t->ncols);
}

/* ---- SELECT: aggregate path --------------------------------------------- */

typedef struct {
    int64_t count;  /* COUNT result, and SUM/AVG denominator (non-null count) */
    int64_t isum;   /* SUM accumulator */
    double  dsum;   /* AVG accumulator */
    Value   best;   /* MIN/MAX accumulator (owned when seen) */
    bool    seen;   /* whether best holds a value */
} AggAcc;

static const char *agg_name(AggFunc f) {
    switch (f) {
        case AGG_COUNT: return "COUNT";
        case AGG_SUM:   return "SUM";
        case AGG_AVG:   return "AVG";
        case AGG_MIN:   return "MIN";
        case AGG_MAX:   return "MAX";
    }
    return "?";
}

static void agg_fold(const Plan *plan, int i, AggAcc *acc, const Value *cells) {
    const AggCall *a = &plan->aggs[i];

    if (a->func == AGG_COUNT) {
        if (a->star || !cells[plan->agg_col[i]].is_null) acc->count++;
        return;
    }

    const Value *v = &cells[plan->agg_col[i]];
    if (v->is_null) return;

    switch (a->func) {
        case AGG_SUM:
            acc->isum += v->as.i;
            acc->count++;
            break;
        case AGG_AVG:
            acc->dsum += (double)v->as.i;
            acc->count++;
            break;
        case AGG_MIN:
            if (!acc->seen || cmp_values(v, &acc->best) < 0) {
                value_free(&acc->best);
                acc->best = value_copy(v);
                acc->seen = true;
            }
            break;
        case AGG_MAX:
            if (!acc->seen || cmp_values(v, &acc->best) > 0) {
                value_free(&acc->best);
                acc->best = value_copy(v);
                acc->seen = true;
            }
            break;
        default:
            break;
    }
}

static char *agg_result_str(const Plan *plan, int i, const AggAcc *acc) {
    const AggCall *a = &plan->aggs[i];
    char buf[64];
    switch (a->func) {
        case AGG_COUNT:
            snprintf(buf, sizeof buf, "%" PRId64, acc->count);
            return strdup(buf);
        case AGG_SUM:
            if (acc->count == 0) return strdup("NULL");
            snprintf(buf, sizeof buf, "%" PRId64, acc->isum);
            return strdup(buf);
        case AGG_AVG:
            if (acc->count == 0) return strdup("NULL");
            snprintf(buf, sizeof buf, "%.6g", acc->dsum / (double)acc->count);
            return strdup(buf);
        case AGG_MIN:
        case AGG_MAX:
            if (!acc->seen) return strdup("NULL");
            return value_to_str(&acc->best);
    }
    return strdup("NULL");
}

static void run_aggregate(const Plan *plan, const Table *t, FILE *out) {
    int n = plan->naggs;
    AggAcc *acc = calloc(n, sizeof(AggAcc));

    Value *scratch = malloc(t->ncols * sizeof(Value));
    TableCursor cur;
    table_cursor_init(&cur, t);
    while (table_cursor_next(&cur, scratch)) {
        Row tmp = {scratch};
        if (eval_pred(plan->filter, t, &tmp))
            for (int i = 0; i < n; i++)
                agg_fold(plan, i, &acc[i], scratch);
        for (int c = 0; c < t->ncols; c++) value_free(&scratch[c]);
    }
    free(scratch);

    char **header = malloc(n * sizeof(char *));
    char ***cells = malloc(sizeof(char **));
    cells[0] = malloc(n * sizeof(char *));
    for (int i = 0; i < n; i++) {
        const AggCall *a = &plan->aggs[i];
        char hbuf[MAX_NAME + 16];
        snprintf(hbuf, sizeof hbuf, "%s(%s)", agg_name(a->func),
                 a->star ? "*" : a->column);
        header[i] = strdup(hbuf);
        cells[0][i] = agg_result_str(plan, i, &acc[i]);
    }

    render(out, n, header, 1, cells);

    for (int i = 0; i < n; i++) {
        free(header[i]);
        free(cells[0][i]);
        value_free(&acc[i].best);
    }
    free(cells[0]);
    free(cells);
    free(header);
    free(acc);
}

static bool exec_select(Database *db, const SelectStmt *sel, FILE *out,
                        char *errbuf, int errcap) {
    Plan plan;
    if (!optimizer_plan_select(db, sel, &plan, errbuf, errcap))
        return false;

    if (plan.is_agg)
        run_aggregate(&plan, plan.table, out);
    else
        run_rows(&plan, plan.table, out);

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
