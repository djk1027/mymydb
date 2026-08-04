#include "database.h"

#include <stdlib.h>
#include <string.h>

Database *database_new(uint32_t id, const char *name, uint32_t block_size) {
    Database *db = calloc(1, sizeof(Database));
    db->obj.id = id;
    db->obj.type = OBJ_DB;
    strncpy(db->name, name, MAX_NAME - 1);
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
    Index *ix = db->indexes;
    while (ix) {
        Index *next = ix->next;
        index_free(ix);
        ix = next;
    }
    param_free(&db->params);
    if (db->pager) pager_close(db->pager);
    free(db->path);
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

/* ---- indexes ------------------------------------------------------------ */

Index *db_find_index(Database *db, const char *name) {
    for (Index *ix = db->indexes; ix; ix = ix->next)
        if (strcmp(ix->name, name) == 0)
            return ix;
    return NULL;
}

Index *db_create_index(Database *db, const char *name, Table *t,
                       const int *cols, const ColType *types, int ncols) {
    if (db_find_index(db, name)) return NULL;
    Index *ix = index_new(db->obj.id, db->next_table_id++, name,
                          t->obj.id, t->name, cols, types, ncols,
                          db->pager, db->block_size);
    ix->next = db->indexes;
    db->indexes = ix;
    return ix;
}

bool db_drop_index(Database *db, const char *name) {
    Index **pp = &db->indexes;
    for (Index *ix = db->indexes; ix; pp = &ix->next, ix = ix->next) {
        if (strcmp(ix->name, name) == 0) {
            *pp = ix->next;
            index_free(ix);
            return true;
        }
    }
    return false;
}

void db_attach_index(Database *db, Index *ix) {
    ix->next = db->indexes;
    db->indexes = ix;
}
