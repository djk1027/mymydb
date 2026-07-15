#ifndef MYMYDB_EXECUTOR_H
#define MYMYDB_EXECUTOR_H

#include "ast.h"
#include "storage.h"

#include <stdio.h>

/*
 * Executes one parsed statement against db, writing any result set / status
 * to out. Returns true on success; on failure returns false and fills errbuf.
 */
bool execute(Database *db, const Stmt *stmt, FILE *out,
             char *errbuf, int errcap);

#endif /* MYMYDB_EXECUTOR_H */
