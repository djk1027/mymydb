#include "executor.h"
#include "parser.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ERRCAP 256
#define CONFIG_FILE "mymy.conf"

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/*
 * Reads mymy.conf if present (key = value, '#' comments). Recognizes
 * `block_size` and `base_path`; unknown keys are ignored.
 */
static void load_config(const char *path, uint32_t *block_size,
                        char *base_path, size_t bpcap) {
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

        char *key = p, *kend = eq - 1;
        while (kend >= key && isspace((unsigned char)*kend)) *kend-- = '\0';

        char *val = eq + 1;
        while (*val && isspace((unsigned char)*val)) val++;
        char *vend = val + strlen(val) - 1;
        while (vend >= val && isspace((unsigned char)*vend)) *vend-- = '\0';
        if (*val == '\'' && vend > val && *vend == '\'') { val++; *vend = '\0'; }

        if (strcmp(key, "block_size") == 0) {
            long v = strtol(val, NULL, 10);
            if (v >= MIN_BLOCK_SIZE && v <= MAX_BLOCK_SIZE)
                *block_size = (uint32_t)v;
        } else if (strcmp(key, "base_path") == 0) {
            snprintf(base_path, bpcap, "%s", val);
        }
    }
    fclose(f);
}

/*
 * Base path precedence: CLI argument > config base_path > $MYMY/mymydb >
 * $HOME/mymydb. Under it live bin/ and data/.
 */
static void resolve_base(char *out, size_t cap, const char *cli_arg,
                         const char *cfg_base) {
    if (cli_arg && cli_arg[0]) { snprintf(out, cap, "%s", cli_arg); return; }
    if (cfg_base && cfg_base[0]) { snprintf(out, cap, "%s", cfg_base); return; }
    const char *mymy = getenv("MYMY");
    if (mymy && mymy[0]) { snprintf(out, cap, "%s/mymydb", mymy); return; }
    const char *home = getenv("HOME");
    if (home && home[0]) { snprintf(out, cap, "%s/mymydb", home); return; }
    snprintf(out, cap, "./mymydb");
}

/* True if the buffer's first word is a meta command (runs without a ';'). */
static bool is_meta_command(const char *s) {
    s = skip_ws(s);
    char word[16];
    int i = 0;
    while (s[i] && !isspace((unsigned char)s[i]) && i < 15) { word[i] = s[i]; i++; }
    word[i] = '\0';
    static const char *metas[] = {"show", "set", "use", "help",
                                  "exit", "quit", "checkpoint"};
    for (size_t k = 0; k < sizeof metas / sizeof *metas; k++)
        if (strcasecmp(word, metas[k]) == 0) return true;
    return false;
}

/* Runs one statement. Returns false only for EXIT/QUIT (stop the REPL). */
static bool run_sql(Instance *inst, const char *sql) {
    if (*skip_ws(sql) == '\0') return true;

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
    char cfg_base[512] = "";
    load_config(CONFIG_FILE, &block_size, cfg_base, sizeof cfg_base);

    char base[1024];
    resolve_base(base, sizeof base, argc > 1 ? argv[1] : NULL, cfg_base);

    Instance *db = instance_open(base, block_size);
    if (!db) {
        fprintf(stderr, "error: cannot open database at '%s'\n", base);
        return 1;
    }
    bool interactive = isatty(fileno(stdin));

    if (interactive) {
        printf("mymydb v2.2  (base: %s, block %u)\n", base, block_size);
        printf("Statements end with ';'; meta commands (SHOW, USE, HELP, EXIT, "
               "...) run on Enter. Type HELP for help.\n");
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
        if (c == '\n' && !in_string) {
            if (buf) buf[len] = '\0';
            const char *start = buf ? skip_ws(buf) : "";

            if (*start == '\0') { /* empty / whitespace-only buffer (v1.3) */
                len = 0;
                if (swallow_newline) { swallow_newline = false; continue; }
                if (interactive) { printf("mymydb> "); fflush(stdout); }
                continue;
            }

            /* meta commands execute on Enter, without a trailing ';' (v2.2) */
            if (is_meta_command(start)) {
                bool keep = run_sql(db, buf);
                len = 0;
                if (!keep) break;
                if (interactive) { printf("mymydb> "); fflush(stdout); }
                swallow_newline = false;
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
