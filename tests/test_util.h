#ifndef MYMYDB_TEST_UTIL_H
#define MYMYDB_TEST_UTIL_H

/* Tiny zero-dependency test framework. */

#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int checks;
    int failed;
    const char *cur; /* current test name */
} TestStats;

extern TestStats T;

#define RUN(fn) do {                     \
    T.cur = #fn;                         \
    int before = T.failed;               \
    fn();                                \
    printf("  %-28s %s\n", #fn,          \
           T.failed == before ? "ok" : "FAIL"); \
} while (0)

#define CHECK(cond) do {                                       \
    T.checks++;                                                \
    if (!(cond)) {                                             \
        T.failed++;                                            \
        fprintf(stderr, "    FAIL %s:%d in %s: %s\n",          \
                __FILE__, __LINE__, T.cur, #cond);             \
    }                                                          \
} while (0)

/* Substring convenience for asserting on result output. */
static inline int contains(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

/*
 * Parse + execute one statement against db.
 * On success returns true; if out != NULL, *out receives the malloc'd result
 * text (caller frees). On failure returns false and writes the message to err.
 */
static inline bool db_exec(Instance *db, const char *sql,
                           char **out, char *err, size_t errcap) {
    if (out) *out = NULL;
    if (err && errcap) err[0] = '\0';

    char perr[256];
    Stmt *s = parse_statement(sql, perr, sizeof perr);
    if (!s) {
        if (err && errcap) snprintf(err, errcap, "%s", perr);
        return false;
    }

    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    char eerr[256];
    bool ok = execute(db, s, f, eerr, sizeof eerr);
    fclose(f);
    stmt_free(s);

    if (out) *out = buf; else free(buf);
    if (!ok && err && errcap) snprintf(err, errcap, "%s", eerr);
    return ok;
}

#endif /* MYMYDB_TEST_UTIL_H */
