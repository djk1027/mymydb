#include "optimizer.h"

#include <stdio.h>
#include <stdlib.h>

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

    out->kind = PLAN_FULL_SCAN; /* v0: always a full scan */
    out->table = t;
    out->filter = sel->where;
    out->proj = NULL;
    out->nproj = 0;

    if (sel->select_all) {
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
                free(out->proj);
                out->proj = NULL;
                return false;
            }
            out->proj[i] = idx;
        }
    }
    return true;
}

void plan_free(Plan *plan) {
    if (!plan) return;
    free(plan->proj);
    plan->proj = NULL;
}
