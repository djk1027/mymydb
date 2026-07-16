#include "test_util.h"

#include <inttypes.h>

TestStats T = {0};

/* ---- basics ------------------------------------------------------------- */

static void test_create_insert_select(void) {
    Database *db = db_new();
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
    Database *db = db_new();
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
    Database *db = db_new();
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
    Database *db = db_new();
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
    Database *db = db_new();
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

/* ---- pages: rows survive a serialize/deserialize round-trip ------------- */

static void test_pages(void) {
    Database *db = db_new();
    char err[256];
    char *out = NULL;

    db_exec(db, "CREATE TABLE pg (id INT, s TEXT)", NULL, err, sizeof err);

    /* Enough rows to span many 4KB pages, mixing short and long text. */
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

    /* exact text round-trips out of the page */
    CHECK(db_exec(db, "SELECT s FROM pg WHERE id = 4999", &out, err, sizeof err));
    CHECK(contains(out, "value-4999-padding-padding"));
    CHECK(contains(out, "(1 row)"));
    free(out);

    /* a row larger than one page is rejected, not truncated */
    {
        char *big = malloc(6000);
        memset(big, 'x', 5999);
        big[5999] = '\0';
        char *sql = malloc(6100);
        snprintf(sql, 6100, "INSERT INTO pg VALUES (1, '%s')", big);
        CHECK(!db_exec(db, sql, NULL, err, sizeof err));
        CHECK(contains(err, "page"));
        free(sql);
        free(big);
    }

    db_free(db);
}

/* ---- error handling ----------------------------------------------------- */

static void test_errors(void) {
    Database *db = db_new();
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
    Database *db = db_new();
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
    Database *db = db_new();
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
    Database *db = db_new();
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
        Database *db = db_new();
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
    RUN(test_pages);
    RUN(test_errors);
    RUN(test_overflow);
    RUN(test_string_escape);
    RUN(test_large_insert_select);
    RUN(test_many_tables);

    printf("\n%d checks, %d failed\n", T.checks, T.failed);
    return T.failed ? 1 : 0;
}
