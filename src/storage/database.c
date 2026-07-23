#include "database.h"

#include <stdlib.h>
#include <string.h>

Database *database_new(uint32_t id, const char *name, Pager *pager,
                       uint32_t block_size) {
    Database *db = calloc(1, sizeof(Database));
    db->obj.id = id;
    db->obj.type = OBJ_DB;
    strncpy(db->name, name, MAX_NAME - 1);
    db->pager = pager;
    db->block_size = block_size;
    db->next_table_id = 1;
    return db;
}

void database_free(Database *db) {
    Table *t = db->tables;
    while (t) {
        Table *next = t->next;
        table_free(t);
        t = next;
    }
    param_free(&db->params);
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

    Table *t = table_new(db->obj.id, db->next_table_id++, name,
                         cols, ncols, db->pager, db->block_size);
    t->next = db->tables;
    db->tables = t;
    return t;
}

void db_attach_table(Database *db, Table *t) {
    t->next = db->tables;
    db->tables = t;
}
