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
    RUN(test_errors);
    RUN(test_overflow);
    RUN(test_string_escape);
    RUN(test_large_insert_select);
    RUN(test_many_tables);

    printf("\n%d checks, %d failed\n", T.checks, T.failed);
    return T.failed ? 1 : 0;
}
