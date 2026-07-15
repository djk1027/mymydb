#ifndef MYMYDB_PARSER_H
#define MYMYDB_PARSER_H

#include "ast.h"

/*
 * Parses a single SQL statement (trailing ';' optional).
 * Returns a heap Stmt on success (free with stmt_free), or NULL on error.
 * On error, a human-readable message is written into errbuf.
 */
Stmt *parse_statement(const char *sql, char *errbuf, int errcap);

#endif /* MYMYDB_PARSER_H */
