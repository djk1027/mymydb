#include "optimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Verify every column referenced in the WHERE tree exists in the table. */
static bool check_expr_cols(const Table *t, const Expr *e,
                            char *errbuf, int errcap) {
    if (!e) return true;
    switch (e->kind) {
        case EXPR_LITERAL:
            return true;
        case EXPR_COLUMN:
            if (table_col_index(t, e->as.column) < 0) {
                snprintf(errbuf, errcap, "unknown column '%s' in WHERE",
                         e->as.column);
                return false;
            }
            return true;
        case EXPR_BINARY:
            return check_expr_cols(t, e->as.binary.left, errbuf, errcap) &&
                   check_expr_cols(t, e->as.binary.right, errbuf, errcap);
    }
    return true;
}

bool optimizer_check_where(const Table *t, const Expr *where,
                           char *errbuf, int errcap) {
    return check_expr_cols(t, where, errbuf, errcap);
}

/* ---- index selection (v2.3) --------------------------------------------- */

static bool is_cmp(OpKind op) {
    return op == OP_EQ || op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE;
}

/* Collects top-level AND-ed comparisons; sets *has_or if any OR is present
 * (which makes the predicate unusable for a single-index scan). */
static void gather_conjuncts(const Expr *e, const Expr **out, int *n, int cap,
                             bool *has_or) {
    if (!e || e->kind != EXPR_BINARY) return;
    OpKind op = e->as.binary.op;
    if (op == OP_AND) {
        gather_conjuncts(e->as.binary.left, out, n, cap, has_or);
        gather_conjuncts(e->as.binary.right, out, n, cap, has_or);
    } else if (op == OP_OR) {
        *has_or = true;
    } else if (is_cmp(op) && *n < cap) {
        out[(*n)++] = e;
    }
}

/* Marks (in used[]) every table column referenced anywhere in the predicate. */
static void mark_where_cols(const Table *t, const Expr *e, bool *used) {
    if (!e) return;
    if (e->kind == EXPR_COLUMN) {
        int idx = table_col_index(t, e->as.column);
        if (idx >= 0) used[idx] = true;
    } else if (e->kind == EXPR_BINARY) {
        mark_where_cols(t, e->as.binary.left, used);
        mark_where_cols(t, e->as.binary.right, used);
    }
}

/* Chooses an index scan for the plan if a top-level comparison drives one of
 * db's indexes on the plan's table (=/range on the index's first column). */
static void choose_index(Database *db, const Table *t, const SelectStmt *sel,
                         Plan *out) {
    if (!sel->where) return;

    const Expr *conj[32];
    int nconj = 0;
    bool has_or = false;
    gather_conjuncts(sel->where, conj, &nconj, 32, &has_or);
    if (has_or || nconj == 0) return;

    for (int c = 0; c < nconj; c++) {
        const Expr *cmp = conj[c];
        const Expr *col = cmp->as.binary.left;
        const Expr *lit = cmp->as.binary.right;
        if (col->kind != EXPR_COLUMN || lit->kind != EXPR_LITERAL) continue;
        int tcol = table_col_index(t, col->as.column);
        if (tcol < 0 || lit->as.literal.is_null) continue;
        if (lit->as.literal.type != t->cols[tcol].type) continue;

        for (Index *ix = db->indexes; ix; ix = ix->next) {
            if (ix->table_id != t->obj.id) continue;
            if (ix->cols[0] != tcol) continue; /* drive on the first index column */

            out->use_index = true;
            out->index = ix;
            out->idx_driving = tcol;
            out->idx_op = cmp->as.binary.op;
            out->idx_bound = lit->as.literal;

            /* covering: every column the query needs is in the index */
            bool need[64] = {false};
            int nt = t->ncols < 64 ? t->ncols : 64;
            mark_where_cols(t, sel->where, need);
            if (!sel->is_agg && !sel->select_all)
                for (int i = 0; i < out->nproj; i++)
                    if (out->proj[i] < 64) need[out->proj[i]] = true;
            bool covering = !sel->select_all && !sel->is_agg;
            if (covering)
                for (int i = 0; i < nt; i++)
                    if (need[i] && index_col_pos(ix, i) < 0) { covering = false; break; }
            out->covering = covering;
            return;
        }
    }
}

bool optimizer_plan_select(Database *db, const SelectStmt *sel,
                           Plan *out, char *errbuf, int errcap) {
    Table *t = db_find_table(db, sel->table);
    if (!t) {
        snprintf(errbuf, errcap, "no such table: %s", sel->table);
        return false;
    }

    if (!check_expr_cols(t, sel->where, errbuf, errcap))
        return false;

    memset(out, 0, sizeof(*out));
    out->kind = PLAN_FULL_SCAN; /* v0: always a full scan */
    out->table = t;
    out->filter = sel->where;

    /* Resolve aggregates or projection. */
    if (sel->is_agg) {
        out->is_agg = true;
        out->aggs = sel->aggs;
        out->naggs = sel->naggs;
        out->agg_col = malloc(sel->naggs * sizeof(int));
        for (int i = 0; i < sel->naggs; i++) {
            const AggCall *a = &sel->aggs[i];
            if (a->star) {                 /* COUNT(*) */
                out->agg_col[i] = -1;
                continue;
            }
            int idx = table_col_index(t, a->column);
            if (idx < 0) {
                snprintf(errbuf, errcap, "unknown column '%s'", a->column);
                plan_free(out);
                return false;
            }
            if ((a->func == AGG_SUM || a->func == AGG_AVG) &&
                t->cols[idx].type != TYPE_INT) {
                snprintf(errbuf, errcap,
                         "SUM/AVG requires an INT column, but '%s' is %s",
                         a->column, coltype_name(t->cols[idx].type));
                plan_free(out);
                return false;
            }
            out->agg_col[i] = idx;
        }
    } else if (sel->select_all) {
        out->nproj = t->ncols;
        out->proj = malloc(t->ncols * sizeof(int));
        for (int i = 0; i < t->ncols; i++) out->proj[i] = i;
    } else {
        out->nproj = sel->ncols;
        out->proj = malloc(sel->ncols * sizeof(int));
        for (int i = 0; i < sel->ncols; i++) {
            int idx = table_col_index(t, sel->cols[i]);
            if (idx < 0) {
                snprintf(errbuf, errcap, "unknown column '%s'", sel->cols[i]);
                plan_free(out);
                return false;
            }
            out->proj[i] = idx;
        }
    }

    /* Resolve ORDER BY keys against the table's columns. */
    if (sel->norder > 0) {
        out->norder = sel->norder;
        out->order_col = malloc(sel->norder * sizeof(int));
        out->order_desc = malloc(sel->norder * sizeof(bool));
        for (int i = 0; i < sel->norder; i++) {
            int idx = table_col_index(t, sel->order[i].column);
            if (idx < 0) {
                snprintf(errbuf, errcap, "unknown column '%s' in ORDER BY",
                         sel->order[i].column);
                plan_free(out);
                return false;
            }
            out->order_col[i] = idx;
            out->order_desc[i] = sel->order[i].desc;
        }
    }

    choose_index(db, t, sel, out);
    return true;
}

void plan_free(Plan *plan) {
    if (!plan) return;
    free(plan->proj);
    free(plan->agg_col);
    free(plan->order_col);
    free(plan->order_desc);
    plan->proj = NULL;
    plan->agg_col = NULL;
    plan->order_col = NULL;
    plan->order_desc = NULL;
}
