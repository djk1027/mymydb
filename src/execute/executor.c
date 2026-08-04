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

/* Keeps every index on t in sync with a row insert/delete. */
static void indexes_add(Database *db, const Table *t, const Value *cells,
                        RowLoc loc) {
    for (Index *ix = db->indexes; ix; ix = ix->next)
        if (ix->table_id == t->obj.id) index_insert(ix, cells, loc);
}
static void indexes_remove(Database *db, const Table *t, const Value *cells,
                           RowLoc loc) {
    for (Index *ix = db->indexes; ix; ix = ix->next)
        if (ix->table_id == t->obj.id) index_delete(ix, cells, loc);
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

    /* Block storage copies the row in; we always own the transient cells. */
    int blk, slot;
    bool stored = table_append_row_loc(t, cells, &blk, &slot);
    if (stored)
        indexes_add(db, t, cells, (RowLoc){.block = (uint32_t)blk,
                                           .slot = (uint16_t)slot});
    for (int i = 0; i < t->ncols; i++) value_free(&cells[i]);
    free(cells);

    if (!stored) {
        snprintf(errbuf, errcap, "row too large to fit in a %u-byte block",
                 t->block_size);
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

static void free_cells(Value *cells, int n) {
    for (int i = 0; i < n; i++) value_free(&cells[i]);
}

/* Compares a possibly-null driving column value against a non-null bound. */
static int drv_cmp(const Value *v, const Value *bound) {
    if (v->is_null) return -1;
    if (v->type != bound->type) return 0;
    return cmp_values(v, bound);
}

/*
 * Iterates rows matching the plan's filter, calling fn(cells, ctx) for each.
 * Uses the chosen index (point/range on its first column, with the full filter
 * re-checked) when plan->use_index, otherwise a full table scan. For a covering
 * index scan the row is served from the index leaf without reading data blocks.
 * The cells passed to fn are only valid for the duration of the call.
 */
typedef void (*RowFn)(const Value *cells, void *ctx);

static void foreach_match(const Plan *plan, const Table *t, RowFn fn, void *ctx) {
    int ncols = t->ncols;

    if (!plan->use_index) {
        Value *scratch = malloc(ncols * sizeof(Value));
        TableCursor cur;
        table_cursor_init(&cur, t);
        while (table_cursor_next(&cur, scratch)) {
            Row r = {scratch};
            if (eval_pred(plan->filter, t, &r)) fn(scratch, ctx);
            free_cells(scratch, ncols);
        }
        free(scratch);
        return;
    }

    Index *ix = plan->index;
    OpKind op = plan->idx_op;
    const Value *bound = &plan->idx_bound;

    /* Start the scan at the bound for =,>,>=; at the smallest key for <,<=. */
    Value *lowrow = NULL;
    if (op == OP_EQ || op == OP_GT || op == OP_GE) {
        lowrow = malloc(ncols * sizeof(Value));
        for (int c = 0; c < ncols; c++) lowrow[c] = value_null(t->cols[c].type);
        lowrow[plan->idx_driving] = *bound; /* borrowed */
    }
    IndexCursor c;
    index_scan_begin(ix, &c, lowrow, lowrow ? 1 : 0);

    Value *keycells = malloc(ix->ncols * sizeof(Value));
    Value *rowbuf = malloc(ncols * sizeof(Value));
    RowLoc loc;
    while (index_scan_next(&c, &loc, keycells)) {
        int cmp = drv_cmp(&keycells[0], bound);
        if (op == OP_EQ && cmp != 0) { free_cells(keycells, ix->ncols); break; }
        if ((op == OP_LT || op == OP_LE) && cmp > 0) {
            free_cells(keycells, ix->ncols);
            break;
        }

        const Value *rowcells = NULL;
        bool owned = false;
        if (plan->covering) {
            for (int cc = 0; cc < ncols; cc++)
                rowbuf[cc] = value_null(t->cols[cc].type);
            for (int i = 0; i < ix->ncols; i++)
                rowbuf[ix->cols[i]] = keycells[i]; /* borrow */
            rowcells = rowbuf;
        } else if (table_read_at(t, (int)loc.block, (int)loc.slot, rowbuf)) {
            rowcells = rowbuf;
            owned = true;
        }
        if (rowcells) {
            Row r = {(Value *)rowcells};
            if (eval_pred(plan->filter, t, &r)) fn(rowcells, ctx);
        }
        if (owned) free_cells(rowbuf, ncols);
        free_cells(keycells, ix->ncols);
    }
    free(keycells);
    free(rowbuf);
    free(lowrow);
}

/* Collects every matching row as an owned materialized-row vector. */
typedef struct { Row *rows; int n, cap, ncols; } RowVec;

static void collect_fn(const Value *cells, void *ctx) {
    RowVec *v = ctx;
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->rows = realloc(v->rows, v->cap * sizeof(Row));
    }
    Value *copy = malloc(v->ncols * sizeof(Value));
    for (int c = 0; c < v->ncols; c++) copy[c] = value_copy(&cells[c]);
    v->rows[v->n++].cells = copy;
}

static Row *scan_rows(const Plan *plan, const Table *t, int *out_n) {
    RowVec v = {0};
    v.ncols = t->ncols;
    foreach_match(plan, t, collect_fn, &v);
    *out_n = v.n;
    return v.rows;
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

typedef struct { const Plan *plan; AggAcc *acc; int n; } AggCtx;

static void agg_fn(const Value *cells, void *ctx) {
    AggCtx *a = ctx;
    for (int i = 0; i < a->n; i++)
        agg_fold(a->plan, i, &a->acc[i], cells);
}

static void run_aggregate(const Plan *plan, const Table *t, FILE *out) {
    int n = plan->naggs;
    AggAcc *acc = calloc(n, sizeof(AggAcc));

    AggCtx ctx = {.plan = plan, .acc = acc, .n = n};
    foreach_match(plan, t, agg_fn, &ctx);

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

/* ---- DELETE ------------------------------------------------------------- */

static bool exec_delete(Database *db, const DeleteStmt *del, FILE *out,
                        char *errbuf, int errcap) {
    Table *t = db_find_table(db, del->table);
    if (!t) {
        snprintf(errbuf, errcap, "no such table: %s", del->table);
        return false;
    }
    if (!optimizer_check_where(t, del->where, errbuf, errcap))
        return false;

    Value *scratch = malloc(t->ncols * sizeof(Value));
    TableCursor cur;
    table_cursor_init(&cur, t);
    int deleted = 0;
    while (table_cursor_next(&cur, scratch)) {
        Row tmp = {scratch};
        bool match = eval_pred(del->where, t, &tmp);
        if (match) {
            /* Remove index entries first (needs the row's values), then
             * tombstone. Doing so mid-scan is safe: slots don't shift and the
             * cursor has advanced past this row. */
            indexes_remove(db, t, scratch,
                           (RowLoc){.block = (uint32_t)cur.cur_block,
                                    .slot = (uint16_t)cur.cur_slot});
            table_delete_at(t, cur.cur_block, cur.cur_slot);
            deleted++;
        }
        for (int c = 0; c < t->ncols; c++) value_free(&scratch[c]);
    }
    free(scratch);

    fprintf(out, "OK: %d row%s deleted from '%s'\n",
            deleted, deleted == 1 ? "" : "s", t->name);
    return true;
}

/* ---- UPDATE ------------------------------------------------------------- */

static bool exec_update(Database *db, const UpdateStmt *up, FILE *out,
                        char *errbuf, int errcap) {
    Table *t = db_find_table(db, up->table);
    if (!t) {
        snprintf(errbuf, errcap, "no such table: %s", up->table);
        return false;
    }
    if (!optimizer_check_where(t, up->where, errbuf, errcap))
        return false;

    /* Resolve SET targets and coerce their values to the column types. */
    int *setcol = malloc(up->nset * sizeof(int));
    Value *setval = malloc(up->nset * sizeof(Value));
    for (int i = 0; i < up->nset; i++) {
        int idx = table_col_index(t, up->cols[i]);
        if (idx < 0) {
            snprintf(errbuf, errcap, "unknown column '%s'", up->cols[i]);
            for (int j = 0; j < i; j++) value_free(&setval[j]);
            free(setcol); free(setval);
            return false;
        }
        if (!coerce(&up->vals[i], t->cols[idx].type, &setval[i],
                    errbuf, errcap)) {
            for (int j = 0; j < i; j++) value_free(&setval[j]);
            free(setcol); free(setval);
            return false;
        }
        setcol[i] = idx;
    }

    /* Pass 1: collect matching row locations (so newly appended rows are not
     * revisited by the scan). */
    RowLoc *locs = NULL;
    int nloc = 0, cap = 0;
    Value *scratch = malloc(t->ncols * sizeof(Value));
    TableCursor cur;
    table_cursor_init(&cur, t);
    while (table_cursor_next(&cur, scratch)) {
        Row tmp = {scratch};
        if (eval_pred(up->where, t, &tmp)) {
            if (nloc == cap) {
                cap = cap ? cap * 2 : 16;
                locs = realloc(locs, cap * sizeof(RowLoc));
            }
            locs[nloc++] = (RowLoc){.block = (uint32_t)cur.cur_block,
                                    .slot = (uint16_t)cur.cur_slot};
        }
        for (int c = 0; c < t->ncols; c++) value_free(&scratch[c]);
    }
    free(scratch);

    /* Pass 2: rewrite each matched row (delete + re-append) and re-key indexes. */
    int updated = 0;
    Value *oldc = malloc(t->ncols * sizeof(Value));
    Value *newc = malloc(t->ncols * sizeof(Value));
    for (int i = 0; i < nloc; i++) {
        if (!table_read_at(t, (int)locs[i].block, (int)locs[i].slot, oldc))
            continue;
        for (int c = 0; c < t->ncols; c++) newc[c] = value_copy(&oldc[c]);
        for (int s = 0; s < up->nset; s++) {
            value_free(&newc[setcol[s]]);
            newc[setcol[s]] = value_copy(&setval[s]);
        }
        indexes_remove(db, t, oldc, locs[i]);
        table_delete_at(t, (int)locs[i].block, (int)locs[i].slot);
        int nb, ns;
        if (table_append_row_loc(t, newc, &nb, &ns))
            indexes_add(db, t, newc,
                        (RowLoc){.block = (uint32_t)nb, .slot = (uint16_t)ns});
        free_cells(oldc, t->ncols);
        free_cells(newc, t->ncols);
        updated++;
    }
    free(oldc); free(newc); free(locs);
    for (int i = 0; i < up->nset; i++) value_free(&setval[i]);
    free(setcol); free(setval);

    fprintf(out, "OK: %d row%s updated in '%s'\n",
            updated, updated == 1 ? "" : "s", t->name);
    return true;
}

/* ---- CREATE INDEX / DROP INDEX ------------------------------------------ */

static bool exec_create_index(Database *db, const CreateIndexStmt *ci, FILE *out,
                              char *errbuf, int errcap) {
    Table *t = db_find_table(db, ci->table);
    if (!t) {
        snprintf(errbuf, errcap, "no such table: %s", ci->table);
        return false;
    }
    int *cols = malloc(ci->ncols * sizeof(int));
    ColType *types = malloc(ci->ncols * sizeof(ColType));
    for (int i = 0; i < ci->ncols; i++) {
        int idx = table_col_index(t, ci->cols[i]);
        if (idx < 0) {
            snprintf(errbuf, errcap, "unknown column '%s'", ci->cols[i]);
            free(cols); free(types);
            return false;
        }
        cols[i] = idx;
        types[i] = t->cols[idx].type;
    }

    Index *ix = db_create_index(db, ci->name, t, cols, types, ci->ncols);
    free(cols); free(types);
    if (!ix) {
        snprintf(errbuf, errcap, "index already exists: %s", ci->name);
        return false;
    }

    /* Populate the index from the existing rows. */
    Value *scratch = malloc(t->ncols * sizeof(Value));
    TableCursor cur;
    table_cursor_init(&cur, t);
    while (table_cursor_next(&cur, scratch)) {
        index_insert(ix, scratch,
                     (RowLoc){.block = (uint32_t)cur.cur_block,
                              .slot = (uint16_t)cur.cur_slot});
        free_cells(scratch, t->ncols);
    }
    free(scratch);

    fprintf(out, "OK: index '%s' created on '%s'\n", ci->name, t->name);
    return true;
}

static bool exec_drop_index(Database *db, const DbStmt *s, FILE *out,
                            char *errbuf, int errcap) {
    if (!db_drop_index(db, s->name)) {
        snprintf(errbuf, errcap, "no such index: %s", s->name);
        return false;
    }
    fprintf(out, "OK: index '%s' dropped\n", s->name);
    return true;
}

/* ---- CREATE DATABASE / USE ---------------------------------------------- */

static bool exec_create_database(Instance *inst, const DbStmt *s, FILE *out,
                                 char *errbuf, int errcap) {
    if (!instance_create_db(inst, s->name)) {
        snprintf(errbuf, errcap, "database already exists: %s", s->name);
        return false;
    }
    fprintf(out, "OK: database '%s' created\n", s->name);
    return true;
}

static bool exec_use(Instance *inst, const DbStmt *s, FILE *out,
                     char *errbuf, int errcap) {
    if (!instance_use(inst, s->name)) {
        snprintf(errbuf, errcap, "no such database: %s", s->name);
        return false;
    }
    fprintf(out, "OK: using database '%s'\n", s->name);
    return true;
}

/* ---- SHOW / SET / HELP (v2.1) ------------------------------------------- */

/* Renders a single borrowed-string column. items[] are not freed. */
static void render_list(FILE *out, const char *header, int n,
                        const char **items) {
    char *hdr = strdup(header);
    char ***cells = malloc((n ? n : 1) * sizeof(char **));
    for (int i = 0; i < n; i++) {
        cells[i] = malloc(sizeof(char *));
        cells[i][0] = (char *)items[i];
    }
    render(out, 1, &hdr, n, cells);
    for (int i = 0; i < n; i++) free(cells[i]);
    free(cells);
    free(hdr);
}

/* Renders a parameter store as a name | value table. */
static void render_params(FILE *out, const ParamStore *s) {
    char *hdr[2] = {strdup("name"), strdup("value")};
    char ***cells = malloc((s->n ? s->n : 1) * sizeof(char **));
    for (int i = 0; i < s->n; i++) {
        cells[i] = malloc(2 * sizeof(char *));
        cells[i][0] = s->items[i].name;
        cells[i][1] = s->items[i].value;
    }
    render(out, 2, hdr, s->n, cells);
    for (int i = 0; i < s->n; i++) free(cells[i]);
    free(cells);
    free(hdr[0]);
    free(hdr[1]);
}

static bool exec_show(Instance *inst, const ShowStmt *sh, FILE *out,
                      char *errbuf, int errcap) {
    Database *db = instance_current(inst);

    switch (sh->kind) {
        case SHOW_DATABASES: {
            int n = 0;
            for (Database *d = inst->databases; d; d = d->next) n++;
            const char **names = malloc((n ? n : 1) * sizeof(char *));
            int i = 0;
            for (Database *d = inst->databases; d; d = d->next) names[i++] = d->name;
            render_list(out, "Database", n, names);
            free(names);
            return true;
        }
        case SHOW_TABLES: {
            int n = 0;
            for (Table *t = db->tables; t; t = t->next) n++;
            const char **names = malloc((n ? n : 1) * sizeof(char *));
            int i = 0;
            for (Table *t = db->tables; t; t = t->next) names[i++] = t->name;
            char header[MAX_NAME + 16];
            snprintf(header, sizeof header, "Tables_in_%s", db->name);
            render_list(out, header, n, names);
            free(names);
            return true;
        }
        case SHOW_PARAMETERS:
            render_params(out, &db->params);
            return true;
        case SHOW_GLOBAL_PARAMETERS:
            render_params(out, &inst->globals);
            return true;
        case SHOW_CREATE_TABLE: {
            Table *t = db_find_table(db, sh->name);
            if (!t) {
                snprintf(errbuf, errcap, "no such table: %s", sh->name);
                return false;
            }
            char ddl[1024];
            int off = snprintf(ddl, sizeof ddl, "CREATE TABLE %s (", t->name);
            for (int c = 0; c < t->ncols && off < (int)sizeof ddl; c++)
                off += snprintf(ddl + off, sizeof ddl - off, "%s%s %s",
                                c ? ", " : "", t->cols[c].name,
                                coltype_name(t->cols[c].type));
            if (off < (int)sizeof ddl) snprintf(ddl + off, sizeof ddl - off, ")");

            char *hdr[2] = {strdup("Table"), strdup("Create Table")};
            char ***cells = malloc(sizeof(char **));
            cells[0] = malloc(2 * sizeof(char *));
            cells[0][0] = t->name;
            cells[0][1] = ddl;
            render(out, 2, hdr, 1, cells);
            free(cells[0]);
            free(cells);
            free(hdr[0]);
            free(hdr[1]);
            return true;
        }
    }
    return true;
}

static bool exec_set(Instance *inst, const SetStmt *st, FILE *out,
                     char *errbuf, int errcap) {
    if (strcmp(st->name, "block_size") == 0) {
        char *end;
        long v = strtol(st->value, &end, 10);
        if (*end != '\0' || v < MIN_BLOCK_SIZE || v > MAX_BLOCK_SIZE) {
            snprintf(errbuf, errcap,
                     "block_size must be an integer in [%d, %d]",
                     MIN_BLOCK_SIZE, MAX_BLOCK_SIZE);
            return false;
        }
    }
    ParamStore *store = st->global ? &inst->globals
                                   : &instance_current(inst)->params;
    param_set(store, st->name, st->value);
    fprintf(out, "OK: %s parameter '%s' = %s\n",
            st->global ? "global" : "database", st->name, st->value);
    return true;
}

static void exec_help(FILE *out) {
    fprintf(out,
        "mymydb v2.2 — commands:\n"
        "  DML/DDL end with ';'; meta commands below run without ';'.\n"
        "  CREATE DATABASE app;   CREATE TABLE t (a INT, b TEXT);\n"
        "  INSERT INTO t VALUES (1, 'hi');\n"
        "  SELECT * FROM t WHERE a >= 1 ORDER BY a DESC;\n"
        "  SELECT COUNT(*), SUM(a) FROM t;\n"
        "  DELETE FROM t WHERE a = 1;\n"
        "  meta:  USE app   SHOW DATABASES   SHOW TABLES   SHOW CREATE TABLE t\n"
        "         SHOW PARAMETERS   SHOW GLOBAL PARAMETERS\n"
        "         SET key = 'value'   SET GLOBAL block_size = 16384\n"
        "         CHECKPOINT   HELP   EXIT\n");
}

/* ---- dispatch ----------------------------------------------------------- */

bool execute(Instance *inst, const Stmt *stmt, FILE *out,
             char *errbuf, int errcap) {
    Database *db = instance_current(inst);

    /* Mutations only update the in-memory buffer; durability comes from an
     * explicit CHECKPOINT or from db_free at shutdown (v2.2). */
    switch (stmt->type) {
        case STMT_CREATE:
            return exec_create(db, &stmt->as.create, out, errbuf, errcap);
        case STMT_INSERT:
            return exec_insert(db, &stmt->as.insert, out, errbuf, errcap);
        case STMT_SELECT:
            return exec_select(db, &stmt->as.select, out, errbuf, errcap);
        case STMT_DELETE:
            return exec_delete(db, &stmt->as.del, out, errbuf, errcap);
        case STMT_UPDATE:
            return exec_update(db, &stmt->as.update, out, errbuf, errcap);
        case STMT_CREATE_INDEX:
            return exec_create_index(db, &stmt->as.create_index, out, errbuf, errcap);
        case STMT_DROP_INDEX:
            return exec_drop_index(db, &stmt->as.db, out, errbuf, errcap);
        case STMT_CREATE_DATABASE:
            return exec_create_database(inst, &stmt->as.db, out, errbuf, errcap);
        case STMT_USE:
            return exec_use(inst, &stmt->as.db, out, errbuf, errcap);
        case STMT_SHOW:
            return exec_show(inst, &stmt->as.show, out, errbuf, errcap);
        case STMT_SET:
            return exec_set(inst, &stmt->as.set, out, errbuf, errcap);
        case STMT_HELP:
            exec_help(out);
            return true;
        case STMT_CHECKPOINT:
            instance_checkpoint(inst);
            fprintf(out, "OK: checkpoint complete\n");
            return true;
        case STMT_EXIT:
            /* The REPL intercepts EXIT before executing; treat as a no-op. */
            return true;
    }
    snprintf(errbuf, errcap, "unknown statement type");
    return false;
}
