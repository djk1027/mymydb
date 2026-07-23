#ifndef MYMYDB_EXECUTOR_H
#define MYMYDB_EXECUTOR_H

#include "ast.h"
#include "storage.h"

#include <stdio.h>

/*
 * Executes one parsed statement against the instance's current database,
 * writing any result set / status to out. A successful mutation (CREATE,
 * INSERT, DELETE, CREATE DATABASE) triggers a checkpoint when the instance is
 * file-backed. Returns true on success; on failure returns false and fills
 * errbuf.
 */
bool execute(Instance *inst, const Stmt *stmt, FILE *out,
             char *errbuf, int errcap);

#endif /* MYMYDB_EXECUTOR_H */
