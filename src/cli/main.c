#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERRCAP 256

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Returns true to continue the REPL, false to quit. */
static bool run_dot_command(Database *db, const char *line) {
    const char *cmd = skip_ws(line);

    if (strcmp(cmd, ".exit") == 0 || strcmp(cmd, ".quit") == 0)
        return false;

    if (strcmp(cmd, ".help") == 0) {
        printf(
            "mymydb v1.3 — commands:\n"
            "  CREATE TABLE t (a INT, b TEXT);\n"
            "  INSERT INTO t VALUES (1, 'hi');\n"
            "  SELECT * FROM t WHERE a >= 1 AND b = 'hi';\n"
            "  SELECT COUNT(*), SUM(a), AVG(a), MIN(a), MAX(a) FROM t;\n"
            "  SELECT a, b FROM t ORDER BY a DESC, b;\n"
            "  .tables            list tables\n"
            "  .schema [table]    show schema\n"
            "  .help / .exit\n");
        return true;
    }

    if (strcmp(cmd, ".tables") == 0) {
        int n = 0;
        for (Table *t = db->tables; t; t = t->next, n++)
            printf("%s\n", t->name);
        if (n == 0) printf("(no tables)\n");
        return true;
    }

    if (strncmp(cmd, ".schema", 7) == 0) {
        const char *arg = skip_ws(cmd + 7);
        for (Table *t = db->tables; t; t = t->next) {
            if (*arg && strcmp(arg, t->name) != 0) continue;
            printf("CREATE TABLE %s (", t->name);
            for (int i = 0; i < t->ncols; i++)
                printf("%s%s %s", i ? ", " : "",
                       t->cols[i].name, coltype_name(t->cols[i].type));
            printf(");\n");
        }
        return true;
    }

    printf("unknown command: %s (try .help)\n", cmd);
    return true;
}

static void run_sql(Database *db, const char *sql) {
    if (*skip_ws(sql) == '\0') return; /* nothing but whitespace */

    char err[ERRCAP] = {0};
    Stmt *stmt = parse_statement(sql, err, ERRCAP);
    if (!stmt) {
        fprintf(stderr, "parse error: %s\n", err);
        return;
    }
    if (!execute(db, stmt, stdout, err, ERRCAP))
        fprintf(stderr, "error: %s\n", err);
    stmt_free(stmt);
}

int main(void) {
    Database *db = db_new();
    bool interactive = isatty(fileno(stdin));

    if (interactive)
        printf("mymydb v1.3. Type .help for help, .exit to quit.\n");

    char *buf = NULL;
    size_t cap = 0, len = 0;
    bool in_string = false;
    /* Set right after a ';' statement runs, so the Enter that terminates it
     * (delivered together with the ';' in a line-buffered terminal) is absorbed
     * instead of drawing a second prompt. */
    bool swallow_newline = false;
    int c;

    if (interactive) { printf("mymydb> "); fflush(stdout); }

    while ((c = getchar()) != EOF) {
        /* On end-of-line, handle dot-commands and blank lines specially. */
        if (c == '\n' && !in_string) {
            if (buf) buf[len] = '\0';
            const char *start = buf ? skip_ws(buf) : "";

            /* dot-command: a line that (ignoring leading space) begins with '.' */
            if (*start == '.') {
                bool keep = run_dot_command(db, buf);
                len = 0;
                if (!keep) break;
                if (interactive) { printf("mymydb> "); fflush(stdout); }
                continue;
            }

            /* Empty / whitespace-only buffer. */
            if (*start == '\0') {
                len = 0;
                if (swallow_newline) {
                    /* This Enter merely terminates a statement that already
                     * reprinted the prompt; don't draw another. */
                    swallow_newline = false;
                    continue;
                }
                /* Genuine blank line: re-show the prompt (v1.3). */
                if (interactive) { printf("mymydb> "); fflush(stdout); }
                continue;
            }
            /* Otherwise we're mid-statement; the newline is ordinary whitespace. */
            swallow_newline = false;
        }

        if (c == '\'') in_string = !in_string;

        if (c == ';' && !in_string) {
            buf[len] = '\0';
            run_sql(db, buf);
            len = 0;
            if (interactive) { printf("mymydb> "); fflush(stdout); }
            swallow_newline = true;
            continue;
        }

        if (len + 2 > cap) {
            cap = cap ? cap * 2 : 128;
            buf = realloc(buf, cap);
        }
        buf[len++] = (char)c;
        /* Real content on the line cancels a pending newline-swallow, so a later
         * blank Enter still gets its own prompt. Whitespace is left pending. */
        if (!isspace((unsigned char)c)) swallow_newline = false;
    }

    /* trailing statement without a final ';' */
    if (len > 0) {
        buf[len] = '\0';
        const char *start = skip_ws(buf);
        if (*start == '.') run_dot_command(db, buf);
        else run_sql(db, buf);
    }

    if (interactive) printf("\n");
    free(buf);
    db_free(db);
    return 0;
}
