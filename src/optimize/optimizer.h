#ifndef MYMYDB_OPTIMIZER_H
#define MYMYDB_OPTIMIZER_H

#include "ast.h"
#include "storage.h"

/*
 * A physical plan for a SELECT. v0 only knows one access method:
 * a full table scan with an optional residual WHERE filter.
 */
typedef enum {
    PLAN_FULL_SCAN,
} PlanKind;

typedef struct {
    PlanKind kind;
    Table *table;         /* resolved target table */
    const Expr *filter;   /* residual predicate, borrowed from the Stmt; may be NULL */

    /* Projection: resolved column indices into the table, or NULL for SELECT *. */
    int *proj;
    int nproj;
} Plan;

/*
 * Builds a plan for the SELECT against db. In v0 the "optimizer" always
 * chooses a full scan. Returns false and fills errbuf on error
 * (missing table / unknown column). Free with plan_free.
 */
bool optimizer_plan_select(Database *db, const SelectStmt *sel,
                           Plan *out, char *errbuf, int errcap);

void plan_free(Plan *plan);

#endif /* MYMYDB_OPTIMIZER_H */
