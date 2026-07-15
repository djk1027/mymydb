#include "storage.h"

#include <stdlib.h>
#include <string.h>

Database *db_new(void) {
    Database *db = calloc(1, sizeof(Database));
    return db;
}

static void table_free(Table *t) {
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++)
            value_free(&t->rows[r].cells[c]);
        free(t->rows[r].cells);
    }
    free(t->rows);
    free(t->cols);
    free(t);
}

void db_free(Database *db) {
    if (!db) return;
    Table *t = db->tables;
    while (t) {
        Table *next = t->next;
        table_free(t);
        t = next;
    }
    free(db);
}

Table *db_find_table(Database *db, const char *name) {
    for (Table *t = db->tables; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t;
    return NULL;
}

Table *db_create_table(Database *db, const char *name,
                       const Column *cols, int ncols) {
    if (db_find_table(db, name)) return NULL;

    Table *t = calloc(1, sizeof(Table));
    strncpy(t->name, name, MAX_NAME - 1);
    t->ncols = ncols;
    t->cols = calloc(ncols, sizeof(Column));
    memcpy(t->cols, cols, ncols * sizeof(Column));

    t->next = db->tables;
    db->tables = t;
    return t;
}

void table_append_row(Table *t, Value *cells) {
    if (t->nrows == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->rows = realloc(t->rows, t->cap * sizeof(Row));
    }
    t->rows[t->nrows].cells = cells;
    t->nrows++;
}

int table_col_index(const Table *t, const char *col_name) {
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, col_name) == 0)
            return i;
    return -1;
}
