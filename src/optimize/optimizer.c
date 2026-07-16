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
