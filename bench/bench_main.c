/*
 * mymydb benchmark runner.
 *
 * Runs a fixed set of workloads against the in-memory engine and reports
 * throughput (rows/sec) and latency (ns/row). Row count is configurable:
 *   ./run_bench [N]        (default 200000)
 */

#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void report(const char *name, long rows, double secs) {
    double rps = secs > 0 ? rows / secs : 0.0;
    double ns  = rows > 0 ? secs * 1e9 / rows : 0.0;
    printf("  %-26s %9ld rows  %8.3f ms  %11.0f rows/s  %7.1f ns/row\n",
           name, rows, secs * 1e3, rps, ns);
}

/* Parse+execute one statement, output discarded. Aborts the bench on error. */
static void must_exec(Instance *db, const char *sql, FILE *sink) {
    char err[256];
    Stmt *s = parse_statement(sql, err, sizeof err);
    if (!s) { fprintf(stderr, "parse error: %s\n(%s)\n", err, sql); exit(1); }
    if (!execute(db, s, sink, err, sizeof err)) {
        fprintf(stderr, "exec error: %s\n(%s)\n", err, sql);
        exit(1);
    }
    stmt_free(s);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? strtol(argv[1], NULL, 10) : 200000;
    if (n <= 0) n = 200000;

    FILE *sink = fopen("/dev/null", "w");
    if (!sink) { perror("open /dev/null"); return 1; }

    printf("mymydb benchmark  (N = %ld rows)\n", n);
    printf("  %-26s %14s %11s %16s %12s\n",
           "workload", "rows", "time", "throughput", "latency");

    Instance *db = db_new();
    must_exec(db, "CREATE TABLE b (id INT, label TEXT)", sink);

    /* --- 1. INSERT throughput (end-to-end: parse + execute) --- */
    {
        double t0 = now_sec();
        for (long i = 0; i < n; i++) {
            char sql[96];
            snprintf(sql, sizeof sql, "INSERT INTO b VALUES (%ld, 'row%ld')", i, i);
            must_exec(db, sql, sink);
        }
        report("insert (parse+exec)", n, now_sec() - t0);
    }

    /* Pre-parse the scan queries once so we measure the engine, not the parser. */
    char perr[256];
    Stmt *scan_all = parse_statement("SELECT id FROM b", perr, sizeof perr);
    Stmt *scan_hit = parse_statement(
        "SELECT id FROM b WHERE id >= 0", perr, sizeof perr);
    Stmt *scan_few = parse_statement(
        "SELECT id, label FROM b WHERE label = 'row42'", perr, sizeof perr);
    Stmt *agg_all = parse_statement(
        "SELECT COUNT(*), SUM(id), AVG(id), MIN(id), MAX(id) FROM b",
        perr, sizeof perr);
    Stmt *order_all = parse_statement(
        "SELECT id FROM b ORDER BY id DESC", perr, sizeof perr);

    const int REPEAT = 5;
    char eerr[256];

    /* --- 2. Full-scan SELECT returning every row --- */
    {
        double t0 = now_sec();
        for (int r = 0; r < REPEAT; r++)
            execute(db, scan_all, sink, eerr, sizeof eerr);
        report("full scan (all rows)", (long)n * REPEAT, now_sec() - t0);
    }

    /* --- 3. Full-scan SELECT with a predicate matching every row --- */
    {
        double t0 = now_sec();
        for (int r = 0; r < REPEAT; r++)
            execute(db, scan_hit, sink, eerr, sizeof eerr);
        report("full scan (WHERE, all)", (long)n * REPEAT, now_sec() - t0);
    }

    /* --- 4. Full-scan SELECT with a highly selective predicate --- */
    {
        double t0 = now_sec();
        for (int r = 0; r < REPEAT; r++)
            execute(db, scan_few, sink, eerr, sizeof eerr);
        report("full scan (WHERE, 1 hit)", (long)n * REPEAT, now_sec() - t0);
    }

    /* --- 5. Aggregate over every row (COUNT/SUM/AVG/MIN/MAX) --- */
    {
        double t0 = now_sec();
        for (int r = 0; r < REPEAT; r++)
            execute(db, agg_all, sink, eerr, sizeof eerr);
        report("aggregate (5 funcs)", (long)n * REPEAT, now_sec() - t0);
    }

    /* --- 6. ORDER BY over every row (full sort) --- */
    {
        double t0 = now_sec();
        for (int r = 0; r < REPEAT; r++)
            execute(db, order_all, sink, eerr, sizeof eerr);
        report("order by (full sort)", (long)n * REPEAT, now_sec() - t0);
    }

    stmt_free(scan_all);
    stmt_free(scan_hit);
    stmt_free(scan_few);
    stmt_free(agg_all);
    stmt_free(order_all);
    db_free(db);
    fclose(sink);
    return 0;
}
