#include "test_util.h"

#include <inttypes.h>
#include <unistd.h>

TestStats T = {0};

/* ---- basics ------------------------------------------------------------- */

static void test_create_insert_select(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    CHECK(db_exec(db, "CREATE TABLE t (a INT, b TEXT)", NULL, err, sizeof err));
    CHECK(db_exec(db, "INSERT INTO t VALUES (1, 'hello')", NULL, err, sizeof err));
    CHECK(db_exec(db, "INSERT INTO t VALUES (2, 'world')", NULL, err, sizeof err));

    CHECK(db_exec(db, "SELECT * FROM t", &out, err, sizeof err));
    CHECK(contains(out, "hello"));
    CHECK(contains(out, "world"));
    CHECK(contains(out, "(2 rows)"));
    free(out);

    db_free(db);
}

static void test_projection(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE p (id INT, name TEXT, age INT)", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO p VALUES (1, 'kim', 30)", NULL, err, sizeof err);

    CHECK(db_exec(db, "SELECT name, age FROM p", &out, err, sizeof err));
    CHECK(contains(out, "name"));
    CHECK(contains(out, "age"));
    CHECK(contains(out, "kim"));
    CHECK(contains(out, "30"));
    CHECK(!contains(out, "id")); /* id column not projected */
    free(out);

    db_free(db);
}

/* ---- WHERE -------------------------------------------------------------- */

static void test_where_operators(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE n (v INT, s TEXT)", NULL, err, sizeof err);
    for (int i = 1; i <= 5; i++) {
        char sql[64];
        snprintf(sql, sizeof sql, "INSERT INTO n VALUES (%d, 'x')", i);
        db_exec(db, sql, NULL, err, sizeof err);
    }

    CHECK(db_exec(db, "SELECT v FROM n WHERE v = 3", &out, err, sizeof err));
    CHECK(contains(out, "(1 row)")); free(out);

    CHECK(db_exec(db, "SELECT v FROM n WHERE v >= 4", &out, err, sizeof err));
    CHECK(contains(out, "(2 rows)")); free(out);

    CHECK(db_exec(db, "SELECT v FROM n WHERE v < 3", &out, err, sizeof err));
    CHECK(contains(out, "(2 rows)")); free(out);

    CHECK(db_exec(db, "SELECT v FROM n WHERE v != 3", &out, err, sizeof err));
    CHECK(contains(out, "(4 rows)")); free(out);

    CHECK(db_exec(db, "SELECT v FROM n WHERE v >= 2 AND v <= 4", &out, err, sizeof err));
    CHECK(contains(out, "(3 rows)")); free(out);

    CHECK(db_exec(db, "SELECT v FROM n WHERE v = 1 OR v = 5", &out, err, sizeof err));
    CHECK(contains(out, "(2 rows)")); free(out);

    CHECK(db_exec(db, "SELECT s FROM n WHERE s = 'x'", &out, err, sizeof err));
    CHECK(contains(out, "(5 rows)")); free(out);

    db_free(db);
}

/* ---- aggregates (v1.2) -------------------------------------------------- */

static void test_aggregates(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE agg (id INT, name TEXT, sal INT)", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO agg VALUES (1, 'alice', 100)", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO agg VALUES (2, 'bob', 80)", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO agg VALUES (3, 'carol', 120)", NULL, err, sizeof err);

    CHECK(db_exec(db, "SELECT COUNT(*) FROM agg", &out, err, sizeof err));
    CHECK(contains(out, "COUNT(*)"));
    CHECK(contains(out, "3"));
    free(out);

    CHECK(db_exec(db, "SELECT SUM(sal), MIN(sal), MAX(sal) FROM agg",
                  &out, err, sizeof err));
    CHECK(contains(out, "300")); /* sum */
    CHECK(contains(out, "80"));  /* min */
    CHECK(contains(out, "120")); /* max */
    free(out);

    CHECK(db_exec(db, "SELECT AVG(sal) FROM agg", &out, err, sizeof err));
    CHECK(contains(out, "100")); /* (100+80+120)/3 */
    free(out);

    /* MIN/MAX over TEXT is lexicographic */
    CHECK(db_exec(db, "SELECT MIN(name), MAX(name) FROM agg", &out, err, sizeof err));
    CHECK(contains(out, "alice"));
    CHECK(contains(out, "carol"));
    free(out);

    /* aggregate honours WHERE */
    CHECK(db_exec(db, "SELECT COUNT(*), SUM(sal) FROM agg WHERE sal >= 100",
                  &out, err, sizeof err));
    CHECK(contains(out, "220")); /* 100 + 120 */
    free(out);

    /* aggregate over an empty result: COUNT 0, others NULL */
    CHECK(db_exec(db, "SELECT COUNT(*), SUM(sal), AVG(sal), MIN(name) FROM agg "
                      "WHERE sal > 1000", &out, err, sizeof err));
    CHECK(contains(out, "0"));
    CHECK(contains(out, "NULL"));
    free(out);

    /* errors: SUM on TEXT, unknown function, mixing agg + plain column */
    CHECK(!db_exec(db, "SELECT SUM(name) FROM agg", NULL, err, sizeof err));
    CHECK(contains(err, "INT"));
    CHECK(!db_exec(db, "SELECT MEDIAN(sal) FROM agg", NULL, err, sizeof err));
    CHECK(contains(err, "unknown function"));
    CHECK(!db_exec(db, "SELECT id, COUNT(*) FROM agg", NULL, err, sizeof err));
    CHECK(contains(err, "mix"));

    db_free(db);
}

/* ---- ORDER BY (v1.2) ---------------------------------------------------- */

static void test_order_by(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE o (v INT, g TEXT)", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO o VALUES (3, 'b')", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO o VALUES (1, 'a')", NULL, err, sizeof err);
    db_exec(db, "INSERT INTO o VALUES (2, 'a')", NULL, err, sizeof err);

    /* ascending: first data row must be v=1 */
    CHECK(db_exec(db, "SELECT v FROM o ORDER BY v", &out, err, sizeof err));
    CHECK(contains(out, "1"));
    { /* the "1" line precedes the "3" line */
        const char *p1 = strstr(out, "1");
        const char *p3 = strstr(out, "3");
        CHECK(p1 && p3 && p1 < p3);
    }
    free(out);

    /* descending: v=3 comes before v=1 */
    CHECK(db_exec(db, "SELECT v FROM o ORDER BY v DESC", &out, err, sizeof err));
    {
        const char *p3 = strstr(out, "3");
        const char *p1 = strstr(out, "1");
        CHECK(p1 && p3 && p3 < p1);
    }
    free(out);

    /* multi-key: g ASC, then v DESC -> a:2, a:1, b:3 */
    CHECK(db_exec(db, "SELECT v, g FROM o ORDER BY g, v DESC", &out, err, sizeof err));
    {
        const char *first = strstr(out, "2");  /* first data value */
        const char *last  = strstr(out, "3");
        CHECK(first && last && first < last);
    }
    free(out);

    /* ORDER BY a non-projected column is allowed */
    CHECK(db_exec(db, "SELECT g FROM o ORDER BY v", &out, err, sizeof err));
    free(out);

    /* unknown ORDER BY column is an error */
    CHECK(!db_exec(db, "SELECT v FROM o ORDER BY zzz", NULL, err, sizeof err));
    CHECK(contains(err, "ORDER BY"));

    db_free(db);
}

/* ---- blocks/extents: rows survive a serialize/deserialize round-trip ---- */

static void test_blocks(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE pg (id INT, s TEXT)", NULL, err, sizeof err);

    /* Enough rows to span many 8KB blocks (and several doubling extents). */
    const int N = 5000;
    bool ok = true;
    for (int i = 0; i < N; i++) {
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO pg VALUES (%d, 'value-%d-padding-padding')", i, i);
        if (!db_exec(db, sql, NULL, err, sizeof err)) { ok = false; break; }
    }
    CHECK(ok);

    CHECK(db_exec(db, "SELECT COUNT(*) FROM pg", &out, err, sizeof err));
    CHECK(contains(out, "5000"));
    free(out);

    /* exact text round-trips out of the block */
    CHECK(db_exec(db, "SELECT s FROM pg WHERE id = 4999", &out, err, sizeof err));
    CHECK(contains(out, "value-4999-padding-padding"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    /* a row larger than one 8KB block is rejected, not truncated */
    {
        char *big = malloc(9000);
        memset(big, 'x', 8999);
        big[8999] = '\0';
        char *sql = malloc(9100);
        snprintf(sql, 9100, "INSERT INTO pg VALUES (1, '%s')", big);
        CHECK(!db_exec(db, sql, NULL, err, sizeof err));
        CHECK(contains(err, "block"));
        free(sql);
        free(big);
    }

    db_free(db);
}

/* ---- DELETE (v2.0) ------------------------------------------------------ */

static void test_delete(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE d (id INT, s TEXT)", NULL, err, sizeof err);
    for (int i = 1; i <= 5; i++) {
        char sql[64];
        snprintf(sql, sizeof sql, "INSERT INTO d VALUES (%d, 'r')", i);
        db_exec(db, sql, NULL, err, sizeof err);
    }

    /* delete a single row via WHERE */
    CHECK(db_exec(db, "DELETE FROM d WHERE id = 3", &out, err, sizeof err));
    CHECK(contains(out, "1 row deleted"));
    free(out);

    CHECK(db_exec(db, "SELECT COUNT(*) FROM d", &out, err, sizeof err));
    CHECK(contains(out, "4"));
    free(out);

    /* the deleted row is really gone; remaining rows intact */
    CHECK(db_exec(db, "SELECT id FROM d ORDER BY id", &out, err, sizeof err));
    CHECK(!contains(out, "3"));
    CHECK(contains(out, "1"));
    CHECK(contains(out, "5"));
    free(out);

    /* a row can still be inserted after deletions */
    CHECK(db_exec(db, "INSERT INTO d VALUES (6, 'r')", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT COUNT(*) FROM d", &out, err, sizeof err));
    CHECK(contains(out, "5"));
    free(out);

    /* range delete: ids 4, 5, 6 (3 was already gone) */
    CHECK(db_exec(db, "DELETE FROM d WHERE id >= 4", &out, err, sizeof err));
    CHECK(contains(out, "3 rows deleted"));
    free(out);

    /* DELETE with no WHERE clears everything */
    CHECK(db_exec(db, "DELETE FROM d", &out, err, sizeof err));
    free(out);
    CHECK(db_exec(db, "SELECT COUNT(*) FROM d", &out, err, sizeof err));
    CHECK(contains(out, "0"));
    free(out);

    /* errors: unknown table / unknown WHERE column */
    CHECK(!db_exec(db, "DELETE FROM nope", NULL, err, sizeof err));
    CHECK(contains(err, "no such table"));
    CHECK(!db_exec(db, "DELETE FROM d WHERE zzz = 1", NULL, err, sizeof err));

    db_free(db);
}

/* ---- multiple databases (v2.0) ------------------------------------------ */

static void test_databases(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    /* a fresh instance starts in the default 'main' database */
    CHECK(db_exec(db, "CREATE TABLE t (a INT)", NULL, err, sizeof err));
    CHECK(db_exec(db, "INSERT INTO t VALUES (1)", NULL, err, sizeof err));

    /* a second database is isolated: its own tables */
    CHECK(db_exec(db, "CREATE DATABASE other", NULL, err, sizeof err));
    CHECK(db_exec(db, "USE other", NULL, err, sizeof err));
    CHECK(!db_exec(db, "SELECT * FROM t", NULL, err, sizeof err)); /* not visible here */
    CHECK(contains(err, "no such table"));
    CHECK(db_exec(db, "CREATE TABLE t (b TEXT)", NULL, err, sizeof err));
    CHECK(db_exec(db, "INSERT INTO t VALUES ('x')", NULL, err, sizeof err));

    /* switching back sees the original table */
    CHECK(db_exec(db, "USE main", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT * FROM t", &out, err, sizeof err));
    CHECK(contains(out, "(1 row)"));
    CHECK(contains(out, "a"));
    free(out);

    /* errors: duplicate database / unknown database */
    CHECK(!db_exec(db, "CREATE DATABASE main", NULL, err, sizeof err));
    CHECK(contains(err, "exists"));
    CHECK(!db_exec(db, "USE ghost", NULL, err, sizeof err));
    CHECK(contains(err, "no such database"));

    db_free(db);
}

/* ---- persistence: save + reload across instances (v2.0) ----------------- */

static void test_persistence(void) {
    char err[256];
    char *out = NULL;
    const char *base = "build/persistbase";
    if (system("rm -rf build/persistbase") != 0) { /* ignore */ }

    /* session 1: populate a file-backed instance, then close (checkpoints) */
    {
        Instance *db = instance_open(base, DEFAULT_BLOCK_SIZE);
        CHECK(db != NULL);
        CHECK(db_exec(db, "CREATE DATABASE app", NULL, err, sizeof err));
        CHECK(db_exec(db, "USE app", NULL, err, sizeof err));
        CHECK(db_exec(db, "CREATE TABLE u (id INT, name TEXT)", NULL, err, sizeof err));
        for (int i = 0; i < 300; i++) { /* enough rows to span blocks/extents */
            char sql[96];
            snprintf(sql, sizeof sql, "INSERT INTO u VALUES (%d, 'name%d')", i, i);
            db_exec(db, sql, NULL, err, sizeof err);
        }
        db_exec(db, "DELETE FROM u WHERE id = 150", NULL, err, sizeof err);
        db_free(db);
    }

    /* session 2: reopen the same base and verify everything survived */
    {
        Instance *db = instance_open(base, DEFAULT_BLOCK_SIZE);
        CHECK(db != NULL);
        CHECK(db_exec(db, "USE app", NULL, err, sizeof err));

        CHECK(db_exec(db, "SELECT COUNT(*) FROM u", &out, err, sizeof err));
        CHECK(contains(out, "299")); /* 300 inserted - 1 deleted */
        free(out);

        /* exact values reload correctly, deleted row stays gone */
        CHECK(db_exec(db, "SELECT name FROM u WHERE id = 299", &out, err, sizeof err));
        CHECK(contains(out, "name299"));
        free(out);
        CHECK(db_exec(db, "SELECT id FROM u WHERE id = 150", &out, err, sizeof err));
        CHECK(contains(out, "(0 rows)"));
        free(out);

        /* a further mutation persists too */
        CHECK(db_exec(db, "INSERT INTO u VALUES (1000, 'later')", NULL, err, sizeof err));
        db_free(db);
    }

    /* session 3: confirm the post-reload insert also persisted */
    {
        Instance *db = instance_open(base, DEFAULT_BLOCK_SIZE);
        CHECK(db_exec(db, "USE app", NULL, err, sizeof err));
        CHECK(db_exec(db, "SELECT COUNT(*) FROM u", &out, err, sizeof err));
        CHECK(contains(out, "300"));
        free(out);
        db_free(db);
    }

    if (system("rm -rf build/persistbase") != 0) { /* ignore */ }
}

/* ---- SHOW / parameters (v2.1) ------------------------------------------- */

static void test_show_and_params(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE t (a INT, b TEXT)", NULL, err, sizeof err);

    /* SHOW TABLES / DATABASES / CREATE TABLE (MySQL-style, no dots) */
    CHECK(db_exec(db, "SHOW TABLES", &out, err, sizeof err));
    CHECK(contains(out, "t"));
    free(out);
    CHECK(db_exec(db, "SHOW DATABASES", &out, err, sizeof err));
    CHECK(contains(out, "main"));
    free(out);
    CHECK(db_exec(db, "SHOW CREATE TABLE t", &out, err, sizeof err));
    CHECK(contains(out, "CREATE TABLE t (a INT, b TEXT)"));
    free(out);
    CHECK(!db_exec(db, "SHOW CREATE TABLE nope", NULL, err, sizeof err));
    CHECK(contains(err, "no such table"));

    /* a fresh instance seeds default global params */
    CHECK(db_exec(db, "SHOW GLOBAL PARAMETERS", &out, err, sizeof err));
    CHECK(contains(out, "block_size"));
    CHECK(contains(out, "8192"));
    CHECK(contains(out, "base_path"));
    free(out);

    /* per-db params are seeded from the global defaults on db creation */
    CHECK(db_exec(db, "SHOW PARAMETERS", &out, err, sizeof err));
    CHECK(contains(out, "block_size"));
    free(out);

    /* SET (db-scoped) and SET GLOBAL */
    CHECK(db_exec(db, "SET retention = '30d'", &out, err, sizeof err));
    CHECK(contains(out, "database parameter"));
    free(out);
    CHECK(db_exec(db, "SHOW PARAMETERS", &out, err, sizeof err));
    CHECK(contains(out, "retention"));
    CHECK(contains(out, "30d"));
    free(out);

    CHECK(db_exec(db, "SET GLOBAL block_size = 16384", &out, err, sizeof err));
    CHECK(contains(out, "global parameter"));
    free(out);
    CHECK(db_exec(db, "SHOW GLOBAL PARAMETERS", &out, err, sizeof err));
    CHECK(contains(out, "16384"));
    free(out);

    /* block_size is validated */
    CHECK(!db_exec(db, "SET GLOBAL block_size = 99", NULL, err, sizeof err));
    CHECK(contains(err, "block_size"));

    /* a new database gets the global defaults copied in */
    CHECK(db_exec(db, "CREATE DATABASE d2", NULL, err, sizeof err));
    CHECK(db_exec(db, "USE d2", NULL, err, sizeof err));
    CHECK(db_exec(db, "SHOW PARAMETERS", &out, err, sizeof err));
    CHECK(contains(out, "block_size"));
    CHECK(!contains(out, "retention")); /* per-db param stayed in 'main' */
    free(out);

    /* HELP prints something */
    CHECK(db_exec(db, "HELP", &out, err, sizeof err));
    CHECK(contains(out, "SHOW"));
    free(out);

    /* explicit CHECKPOINT succeeds */
    CHECK(db_exec(db, "CHECKPOINT", &out, err, sizeof err));
    CHECK(contains(out, "checkpoint"));
    free(out);

    db_free(db);
}

/* ---- per-database files (v2.2) ------------------------------------------ */

static void test_perdb_files(void) {
    char err[256];
    const char *base = "build/perdbbase";
    if (system("rm -rf build/perdbbase") != 0) { /* ignore */ }

    {
        Instance *db = instance_open(base, DEFAULT_BLOCK_SIZE);
        db_exec(db, "CREATE DATABASE sales", NULL, err, sizeof err);
        db_exec(db, "CREATE DATABASE hr", NULL, err, sizeof err);
        db_exec(db, "USE sales", NULL, err, sizeof err);
        db_exec(db, "CREATE TABLE t (a INT)", NULL, err, sizeof err);
        db_exec(db, "INSERT INTO t VALUES (1)", NULL, err, sizeof err);
        db_free(db);
    }

    /* each database has its own file under data/, plus the master registry */
    CHECK(access("build/perdbbase/data/_master", F_OK) == 0);
    CHECK(access("build/perdbbase/data/main.mdb", F_OK) == 0);
    CHECK(access("build/perdbbase/data/sales.mdb", F_OK) == 0);
    CHECK(access("build/perdbbase/data/hr.mdb", F_OK) == 0);

    /* the registry + per-db data reload together */
    {
        char *out = NULL;
        Instance *db = instance_open(base, DEFAULT_BLOCK_SIZE);
        CHECK(db_exec(db, "SHOW DATABASES", &out, err, sizeof err));
        CHECK(contains(out, "sales"));
        CHECK(contains(out, "hr"));
        free(out);
        CHECK(db_exec(db, "USE sales", NULL, err, sizeof err));
        CHECK(db_exec(db, "SELECT COUNT(*) FROM t", &out, err, sizeof err));
        CHECK(contains(out, "1"));
        free(out);
        db_free(db);
    }

    if (system("rm -rf build/perdbbase") != 0) { /* ignore */ }
}

/* ---- UPDATE (v2.3) ------------------------------------------------------ */

static void test_update(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE t (id INT, name TEXT, n INT)", NULL, err, sizeof err);
    for (int i = 1; i <= 5; i++) {
        char sql[64];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'r%d', %d)", i, i, i * 10);
        db_exec(db, sql, NULL, err, sizeof err);
    }

    /* single-row, multi-column update */
    CHECK(db_exec(db, "UPDATE t SET name = 'X', n = 999 WHERE id = 3",
                  &out, err, sizeof err));
    CHECK(contains(out, "1 row updated"));
    free(out);
    CHECK(db_exec(db, "SELECT name, n FROM t WHERE id = 3", &out, err, sizeof err));
    CHECK(contains(out, "X"));
    CHECK(contains(out, "999"));
    free(out);

    /* multi-row update via range */
    CHECK(db_exec(db, "UPDATE t SET n = 0 WHERE id >= 4", &out, err, sizeof err));
    CHECK(contains(out, "2 rows updated"));
    free(out);
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE n = 0", &out, err, sizeof err));
    CHECK(contains(out, "2"));
    free(out);

    /* rows not matched are untouched; row count is stable */
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t", &out, err, sizeof err));
    CHECK(contains(out, "5"));
    free(out);

    /* errors */
    CHECK(!db_exec(db, "UPDATE nope SET a = 1", NULL, err, sizeof err));
    CHECK(contains(err, "no such table"));
    CHECK(!db_exec(db, "UPDATE t SET zzz = 1 WHERE id = 1", NULL, err, sizeof err));
    CHECK(contains(err, "unknown column"));

    db_free(db);
}

/* ---- B+tree indexes (v2.3) ---------------------------------------------- */

static void test_indexes(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE t (id INT, grp TEXT, v INT)", NULL, err, sizeof err);
    /* enough rows to force B+tree node splits */
    for (int i = 0; i < 2000; i++) {
        char sql[96];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'g%d', %d)",
                 i, i % 10, i * 2);
        db_exec(db, sql, NULL, err, sizeof err);
    }
    CHECK(db_exec(db, "CREATE INDEX idx_id ON t (id)", NULL, err, sizeof err));
    CHECK(db_exec(db, "CREATE INDEX idx_grp ON t (grp)", NULL, err, sizeof err));

    /* point lookup via index returns the right row */
    CHECK(db_exec(db, "SELECT v FROM t WHERE id = 1234", &out, err, sizeof err));
    CHECK(contains(out, "2468"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    /* range scan */
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE id >= 1990", &out, err, sizeof err));
    CHECK(contains(out, "10"));
    free(out);
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE id <= 4", &out, err, sizeof err));
    CHECK(contains(out, "5"));
    free(out);

    /* text index equality (200 rows share each group) */
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE grp = 'g7'", &out, err, sizeof err));
    CHECK(contains(out, "200"));
    free(out);

    /* covering query: only the indexed column is projected/filtered */
    CHECK(db_exec(db, "SELECT id FROM t WHERE id = 777", &out, err, sizeof err));
    CHECK(contains(out, "777"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    /* index stays correct across INSERT / UPDATE / DELETE */
    CHECK(db_exec(db, "INSERT INTO t VALUES (99999, 'gx', 7)", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT v FROM t WHERE id = 99999", &out, err, sizeof err));
    CHECK(contains(out, "7")); free(out);

    CHECK(db_exec(db, "UPDATE t SET id = 88888 WHERE id = 99999", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE id = 99999", &out, err, sizeof err));
    CHECK(contains(out, "(1 row)")); /* aggregate always yields one row */
    CHECK(contains(out, "0"));
    free(out);
    CHECK(db_exec(db, "SELECT v FROM t WHERE id = 88888", &out, err, sizeof err));
    CHECK(contains(out, "7")); free(out);

    CHECK(db_exec(db, "DELETE FROM t WHERE id = 88888", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT COUNT(*) FROM t WHERE id = 88888", &out, err, sizeof err));
    CHECK(contains(out, "0")); free(out);

    /* DROP INDEX, then the same query still works (via full scan) */
    CHECK(db_exec(db, "DROP INDEX idx_id", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT v FROM t WHERE id = 1234", &out, err, sizeof err));
    CHECK(contains(out, "2468")); free(out);
    CHECK(!db_exec(db, "DROP INDEX idx_id", NULL, err, sizeof err));
    CHECK(contains(err, "no such index"));

    db_free(db);
}

/* ---- performance: full scan vs index scan (v2.3) ------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void perf_run(int N, int reps) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;
    FILE *sink = fopen("/dev/null", "w");

    db_exec(db, "CREATE TABLE t (id INT, v INT)", NULL, err, sizeof err);
    for (int i = 0; i < N; i++) { /* fast path: parse+execute, discard output */
        char sql[64];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, %d)", i, i * 2);
        Stmt *s = parse_statement(sql, err, sizeof err);
        execute(db, s, sink, err, sizeof err);
        stmt_free(s);
    }

    int target = N - 1;
    char q[64];
    snprintf(q, sizeof q, "SELECT v FROM t WHERE id = %d", target);
    char expect[32];
    snprintf(expect, sizeof expect, "%d", target * 2);

    /* full scan (no index yet) */
    double t0 = now_ms();
    for (int r = 0; r < reps; r++) {
        CHECK(db_exec(db, q, &out, err, sizeof err));
        CHECK(contains(out, expect));
        free(out);
    }
    double full_ms = now_ms() - t0;

    db_exec(db, "CREATE INDEX idx_id ON t (id)", NULL, err, sizeof err);

    /* same query, now index-driven */
    t0 = now_ms();
    for (int r = 0; r < reps; r++) {
        CHECK(db_exec(db, q, &out, err, sizeof err));
        CHECK(contains(out, expect));
        free(out);
    }
    double idx_ms = now_ms() - t0;

    printf("    [N=%d, %d reps] full-scan %8.2f ms   index %8.2f ms   speedup %.0fx\n",
           N, reps, full_ms, idx_ms, idx_ms > 0 ? full_ms / idx_ms : 0);
    CHECK(idx_ms < full_ms); /* index must beat a full scan at this scale */

    fclose(sink);
    db_free(db);
}

static void test_perf_100k(void) { perf_run(100000, 50); }
static void test_perf_1m(void)   { perf_run(1000000, 20); }

/* ---- runtime block size + persistence (v2.1) ---------------------------- */

static void test_block_size_persist(void) {
    char err[256];
    char *out = NULL;
    const char *base = "build/bsbase";
    if (system("rm -rf build/bsbase") != 0) { /* ignore */ }

    /* create a database whose file uses a non-default 512-byte block size */
    {
        Instance *db = instance_open(base, 512);
        CHECK(db != NULL);
        db_exec(db, "CREATE TABLE t (id INT, s TEXT)", NULL, err, sizeof err);
        bool ok = true;
        for (int i = 0; i < 300; i++) { /* many small blocks + extents at 512B */
            char sql[96];
            snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'row%d')", i, i);
            if (!db_exec(db, sql, NULL, err, sizeof err)) { ok = false; break; }
        }
        CHECK(ok);
        db_free(db);
    }

    /* reopen requesting a different size: the file's stored 512 must win */
    {
        Instance *db = instance_open(base, 16384);
        CHECK(db != NULL);
        CHECK(db_exec(db, "SHOW GLOBAL PARAMETERS", &out, err, sizeof err));
        CHECK(contains(out, "512"));
        CHECK(!contains(out, "16384"));
        free(out);

        CHECK(db_exec(db, "SELECT COUNT(*) FROM t", &out, err, sizeof err));
        CHECK(contains(out, "300"));
        free(out);
        CHECK(db_exec(db, "SELECT s FROM t WHERE id = 299", &out, err, sizeof err));
        CHECK(contains(out, "row299"));
        free(out);
        db_free(db);
    }

    if (system("rm -rf build/bsbase") != 0) { /* ignore */ }
}

/* ---- error handling ----------------------------------------------------- */

static void test_errors(void) {
    Instance *db = db_new();
    char err[256];

    CHECK(db_exec(db, "CREATE TABLE e (a INT, b TEXT)", NULL, err, sizeof err));

    /* duplicate table */
    CHECK(!db_exec(db, "CREATE TABLE e (a INT)", NULL, err, sizeof err));
    CHECK(contains(err, "exists"));

    /* duplicate column */
    CHECK(!db_exec(db, "CREATE TABLE d (a INT, a TEXT)", NULL, err, sizeof err));
    CHECK(contains(err, "duplicate"));

    /* unknown table */
    CHECK(!db_exec(db, "INSERT INTO nope VALUES (1)", NULL, err, sizeof err));
    CHECK(contains(err, "no such table"));

    /* arity mismatch */
    CHECK(!db_exec(db, "INSERT INTO e VALUES (1)", NULL, err, sizeof err));
    CHECK(contains(err, "columns"));

    /* type mismatch */
    CHECK(!db_exec(db, "INSERT INTO e VALUES ('str', 'b')", NULL, err, sizeof err));
    CHECK(contains(err, "type mismatch"));

    /* unknown column in SELECT */
    CHECK(!db_exec(db, "SELECT zzz FROM e", NULL, err, sizeof err));
    CHECK(contains(err, "unknown column"));

    /* unknown column in WHERE */
    CHECK(!db_exec(db, "SELECT a FROM e WHERE zzz = 1", NULL, err, sizeof err));
    CHECK(contains(err, "WHERE"));

    /* parse errors */
    CHECK(!db_exec(db, "SELCT a FROM e", NULL, err, sizeof err));
    CHECK(!db_exec(db, "CREATE TABLE x (a INT", NULL, err, sizeof err));
    CHECK(!db_exec(db, "INSERT INTO e VALUES (1, 'b'", NULL, err, sizeof err));
    CHECK(!db_exec(db, "", NULL, err, sizeof err));

    db_free(db);
}

/* ---- integer overflow --------------------------------------------------- */

static void test_overflow(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE o (v INT)", NULL, err, sizeof err);

    /* INT64_MAX must be accepted exactly */
    CHECK(db_exec(db, "INSERT INTO o VALUES (9223372036854775807)",
                  NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT v FROM o WHERE v = 9223372036854775807",
                  &out, err, sizeof err));
    CHECK(contains(out, "9223372036854775807"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    /* INT64_MAX + 1 must be rejected at parse time */
    CHECK(!db_exec(db, "INSERT INTO o VALUES (9223372036854775808)",
                   NULL, err, sizeof err));
    CHECK(contains(err, "out of range"));

    /* absurdly long literal must be rejected, not truncated */
    CHECK(!db_exec(db, "INSERT INTO o VALUES (999999999999999999999999999999)",
                   NULL, err, sizeof err));
    CHECK(contains(err, "out of range"));

    db_free(db);
}

/* ---- string escaping ---------------------------------------------------- */

static void test_string_escape(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE q (s TEXT)", NULL, err, sizeof err);
    CHECK(db_exec(db, "INSERT INTO q VALUES ('it''s ok')", NULL, err, sizeof err));
    CHECK(db_exec(db, "SELECT s FROM q WHERE s = 'it''s ok'", &out, err, sizeof err));
    CHECK(contains(out, "it's ok"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    db_free(db);
}

/* ---- scale: large insert / select --------------------------------------- */

#define BIG 100000

static void test_large_insert_select(void) {
    Instance *db = db_new();
    char err[256];
    char *out = NULL;

    CHECK(db_exec(db, "CREATE TABLE big (id INT, label TEXT)", NULL, err, sizeof err));

    bool all_ok = true;
    for (int i = 0; i < BIG; i++) {
        char sql[96];
        snprintf(sql, sizeof sql, "INSERT INTO big VALUES (%d, 'row%d')", i, i);
        if (!db_exec(db, sql, NULL, err, sizeof err)) { all_ok = false; break; }
    }
    CHECK(all_ok);

    /* full scan returning every row */
    CHECK(db_exec(db, "SELECT id FROM big", &out, err, sizeof err));
    CHECK(contains(out, "(100000 rows)"));
    free(out);

    /* WHERE selecting a suffix range: id >= 99990  -> 10 rows */
    CHECK(db_exec(db, "SELECT id FROM big WHERE id >= 99990", &out, err, sizeof err));
    CHECK(contains(out, "(10 rows)"));
    free(out);

    /* WHERE with no matches */
    CHECK(db_exec(db, "SELECT id FROM big WHERE id = 123456789", &out, err, sizeof err));
    CHECK(contains(out, "(0 rows)"));
    free(out);

    db_free(db);
}

/* ---- stability: many tables, repeated create/free ----------------------- */

static void test_many_tables(void) {
    for (int rep = 0; rep < 50; rep++) {
        Instance *db = db_new();
        char err[256];
        for (int i = 0; i < 100; i++) {
            char sql[96];
            snprintf(sql, sizeof sql, "CREATE TABLE t%d (a INT, b TEXT)", i);
            CHECK(db_exec(db, sql, NULL, err, sizeof err));
            snprintf(sql, sizeof sql, "INSERT INTO t%d VALUES (%d, 'v')", i, i);
            CHECK(db_exec(db, sql, NULL, err, sizeof err));
        }
        db_free(db); /* must release everything: valgrind verifies */
    }
}

/* ---- main --------------------------------------------------------------- */

int main(void) {
    printf("running mymydb tests...\n");

    RUN(test_create_insert_select);
    RUN(test_projection);
    RUN(test_where_operators);
    RUN(test_aggregates);
    RUN(test_order_by);
    RUN(test_blocks);
    RUN(test_delete);
    RUN(test_databases);
    RUN(test_persistence);
    RUN(test_show_and_params);
    RUN(test_block_size_persist);
    RUN(test_perdb_files);
    RUN(test_update);
    RUN(test_indexes);
    RUN(test_perf_100k);
    RUN(test_perf_1m);
    RUN(test_errors);
    RUN(test_overflow);
    RUN(test_string_escape);
    RUN(test_large_insert_select);
    RUN(test_many_tables);

    printf("\n%d checks, %d failed\n", T.checks, T.failed);
    return T.failed ? 1 : 0;
}
