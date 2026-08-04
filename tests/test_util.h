#ifndef MYMYDB_TEST_UTIL_H
#define MYMYDB_TEST_UTIL_H

/* Tiny zero-dependency test framework. */

#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int checks;
    int failed;
    const char *cur; /* current test name */
} TestStats;

extern TestStats T;

/* Runs one test, printing its pass/fail and wall-clock time (v2.2). */
#define RUN(fn) do {                                             \
    T.cur = #fn;                                                 \
    int before = T.failed;                                       \
    struct timespec _t0, _t1;                                    \
    clock_gettime(CLOCK_MONOTONIC, &_t0);                        \
    fn();                                                        \
    clock_gettime(CLOCK_MONOTONIC, &_t1);                        \
    double _ms = (_t1.tv_sec - _t0.tv_sec) * 1e3 +               \
                 (_t1.tv_nsec - _t0.tv_nsec) / 1e6;              \
    printf("  %-28s %-4s %9.2f ms\n", #fn,                       \
           T.failed == before ? "ok" : "FAIL", _ms);             \
} while (0)

/*
 * v2.2 removed the in-memory mode, so a test instance is rooted at a fresh
 * temporary base directory. db_new() wipes and reopens it; the on-exit
 * checkpoint from db_free writes into build/testdata (gitignored).
 */
#define TEST_BASE "build/testdata"
static inline Instance *db_new(void) {
    if (system("rm -rf " TEST_BASE) != 0) { /* ignore */ }
    return instance_open(TEST_BASE, DEFAULT_BLOCK_SIZE);
}

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
