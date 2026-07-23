#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERRCAP 256
#define CONFIG_FILE "mymy.conf"

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/*
 * Reads mymy.conf if present (key = value, '#' comments). Recognizes
 * `block_size` and `data_file`; unknown keys are ignored. Missing file or keys
 * leave the caller's defaults untouched.
 */
static void load_config(const char *path, uint32_t *block_size,
                        char *data_file, size_t dfcap) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        /* trim key */
        char *key = p;
        char *kend = eq - 1;
        while (kend >= key && isspace((unsigned char)*kend)) *kend-- = '\0';

        /* trim value (drop surrounding whitespace and optional quotes) */
        char *val = eq + 1;
        while (*val && isspace((unsigned char)*val)) val++;
        char *vend = val + strlen(val) - 1;
        while (vend >= val && isspace((unsigned char)*vend)) *vend-- = '\0';
        if (*val == '\'' && vend > val && *vend == '\'') { val++; *vend = '\0'; }

        if (strcmp(key, "block_size") == 0) {
            long v = strtol(val, NULL, 10);
            if (v >= MIN_BLOCK_SIZE && v <= MAX_BLOCK_SIZE)
                *block_size = (uint32_t)v;
        } else if (strcmp(key, "data_file") == 0) {
            snprintf(data_file, dfcap, "%s", val);
        }
    }
    fclose(f);
}

/* Runs one statement. Returns false only for EXIT/QUIT (stop the REPL). */
static bool run_sql(Instance *inst, const char *sql) {
    if (*skip_ws(sql) == '\0') return true; /* nothing but whitespace */

    char err[ERRCAP] = {0};
    Stmt *stmt = parse_statement(sql, err, ERRCAP);
    if (!stmt) {
        fprintf(stderr, "parse error: %s\n", err);
        return true;
    }

    bool keep = true;
    if (stmt->type == STMT_EXIT)
        keep = false;
    else if (!execute(inst, stmt, stdout, err, ERRCAP))
        fprintf(stderr, "error: %s\n", err);

    stmt_free(stmt);
    return keep;
}

int main(int argc, char **argv) {
    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    char cfg_data_file[512] = "";
    load_config(CONFIG_FILE, &block_size, cfg_data_file, sizeof cfg_data_file);

    /* A CLI path argument wins over the config's data_file. With any path the
     * database is durable; with none it lives only in memory. */
    const char *path = NULL;
    if (argc > 1) path = argv[1];
    else if (cfg_data_file[0]) path = cfg_data_file;

    Instance *db = path ? instance_open(path, block_size)
                        : instance_new_bs(block_size);
    if (!db) {
        fprintf(stderr, "error: cannot open database file '%s'\n", path);
        return 1;
    }
    bool interactive = isatty(fileno(stdin));

    if (interactive) {
        if (path) printf("mymydb v2.1  (data file: %s, block %u)\n",
                         path, block_size);
        else printf("mymydb v2.1  (in-memory, block %u)\n", block_size);
        printf("Type HELP; for help, EXIT; to quit.\n");
    }

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
        /* Blank-line handling on end-of-line (v1.3). */
        if (c == '\n' && !in_string) {
            if (buf) buf[len] = '\0';
            const char *start = buf ? skip_ws(buf) : "";

            if (*start == '\0') { /* empty / whitespace-only buffer */
                len = 0;
                if (swallow_newline) {
                    /* Terminates a statement that already reprinted the prompt. */
                    swallow_newline = false;
                    continue;
                }
                if (interactive) { printf("mymydb> "); fflush(stdout); }
                continue;
            }
            /* Otherwise we're mid-statement; the newline is ordinary whitespace. */
            swallow_newline = false;
        }

        if (c == '\'') in_string = !in_string;

        if (c == ';' && !in_string) {
            buf[len] = '\0';
            bool keep = run_sql(db, buf);
            len = 0;
            if (!keep) break;
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
        run_sql(db, buf);
    }

    if (interactive) printf("\n");
    free(buf);
    db_free(db);
    return 0;
}
