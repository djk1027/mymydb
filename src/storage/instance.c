#include "instance.h"

#include "dbfile.h"
#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char MASTER_MAGIC[8] = {'M','Y','M','Y','M','S','1','\0'};

/* ---- path helpers ------------------------------------------------------- */

static char *join_path(const char *dir, const char *leaf) {
    size_t n = strlen(dir) + 1 + strlen(leaf) + 1;
    char *s = malloc(n);
    snprintf(s, n, "%s/%s", dir, leaf);
    return s;
}

static char *db_file_path(const char *dir, const char *name) {
    size_t n = strlen(dir) + 1 + strlen(name) + 4 + 1;
    char *s = malloc(n);
    snprintf(s, n, "%s/%s.mdb", dir, name);
    return s;
}

/* mkdir -p: create each path component, ignoring "already exists". */
static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);
}

/* ---- master registry ---------------------------------------------------- */

typedef struct { uint32_t id; char name[MAX_NAME]; } DbReg;

static char *master_path(Instance *inst) {
    return join_path(inst->data_dir, "_master");
}

static void master_save(Instance *inst) {
    Buf b = {0};
    buf_params(&b, &inst->globals);
    buf_u32(&b, inst->next_db_id);
    uint32_t ndb = 0;
    for (Database *d = inst->databases; d; d = d->next) ndb++;
    buf_u32(&b, ndb);
    for (Database *d = inst->databases; d; d = d->next) {
        buf_u32(&b, d->obj.id);
        buf_str(&b, d->name);
    }

    char *path = master_path(inst);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(MASTER_MAGIC, 1, 8, f);
        fwrite(b.data, 1, b.len, f);
        fclose(f);
    }
    free(path);
    buf_free(&b);
}

/* Loads the master into inst->globals / next_db_id and returns the db registry
 * (caller frees). Returns false if there is no valid master file. */
static bool master_load(Instance *inst, DbReg **out, int *out_n) {
    char *path = master_path(inst);
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 8) { fclose(f); return false; }

    uint8_t *buf = malloc(size);
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != (size_t)size || memcmp(buf, MASTER_MAGIC, 8) != 0) {
        free(buf);
        return false;
    }

    Rdr r = {.data = buf + 8, .len = (size_t)size - 8, .pos = 0};
    rd_params(&r, &inst->globals);
    inst->next_db_id = rd_u32(&r);
    uint32_t ndb = rd_u32(&r);

    DbReg *regs = malloc((ndb ? ndb : 1) * sizeof(DbReg));
    for (uint32_t i = 0; i < ndb; i++) {
        regs[i].id = rd_u32(&r);
        rd_str(&r, regs[i].name, MAX_NAME);
    }
    free(buf);

    *out = regs;
    *out_n = (int)ndb;
    return true;
}

/* ---- database list ------------------------------------------------------ */

static void attach_db(Instance *inst, Database *db) {
    db->next = NULL;
    if (!inst->databases) { inst->databases = db; return; }
    Database *last = inst->databases;
    while (last->next) last = last->next;
    last->next = db;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void seed_globals(Instance *inst) {
    char bs[32];
    snprintf(bs, sizeof bs, "%u", inst->block_size);
    param_set(&inst->globals, "block_size", bs);
    param_set(&inst->globals, "base_path", inst->base_path);
}

Instance *instance_open(const char *base_path, uint32_t block_size) {
    if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE)
        block_size = DEFAULT_BLOCK_SIZE;

    Instance *inst = calloc(1, sizeof(Instance));
    inst->base_path = strdup(base_path);
    inst->data_dir = join_path(base_path, "data");
    inst->next_db_id = 1;
    inst->block_size = block_size;

    /* base/{bin,data} */
    mkdir_p(inst->base_path);
    char *bin = join_path(inst->base_path, "bin");
    mkdir(bin, 0777);
    free(bin);
    mkdir_p(inst->data_dir);

    DbReg *regs = NULL;
    int nreg = 0;
    if (master_load(inst, &regs, &nreg)) {
        const char *gbs = param_get(&inst->globals, "block_size");
        if (gbs) {
            long v = strtol(gbs, NULL, 10);
            if (v >= MIN_BLOCK_SIZE && v <= MAX_BLOCK_SIZE) inst->block_size = (uint32_t)v;
        }
        for (int i = 0; i < nreg; i++) {
            Database *db = database_new(regs[i].id, regs[i].name, inst->block_size);
            char *path = db_file_path(inst->data_dir, regs[i].name);
            dbfile_open(db, path, inst->block_size);
            free(path);
            attach_db(inst, db);
        }
        free(regs);
        inst->current = instance_find_db(inst, "main");
        if (!inst->current) inst->current = inst->databases;
    } else {
        seed_globals(inst);
        inst->current = instance_create_db(inst, "main");
        master_save(inst);
    }
    return inst;
}

void instance_checkpoint(Instance *inst) {
    for (Database *d = inst->databases; d; d = d->next)
        dbfile_checkpoint(d);
    master_save(inst);
}

void db_free(Instance *inst) {
    if (!inst) return;
    instance_checkpoint(inst);
    Database *d = inst->databases;
    while (d) {
        Database *next = d->next;
        database_free(d);
        d = next;
    }
    param_free(&inst->globals);
    free(inst->base_path);
    free(inst->data_dir);
    free(inst);
}

/* ---- database management ------------------------------------------------ */

Database *instance_current(Instance *inst) { return inst->current; }

Database *instance_find_db(Instance *inst, const char *name) {
    for (Database *d = inst->databases; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

Database *instance_create_db(Instance *inst, const char *name) {
    if (instance_find_db(inst, name)) return NULL;

    Database *db = database_new(inst->next_db_id++, name, inst->block_size);
    char *path = db_file_path(inst->data_dir, name);
    dbfile_open(db, path, inst->block_size);   /* creates + initializes the file */
    free(path);
    param_copy(&db->params, &inst->globals);   /* seed from global defaults */
    attach_db(inst, db);
    master_save(inst);                         /* register the new database */
    return db;
}

bool instance_use(Instance *inst, const char *name) {
    Database *db = instance_find_db(inst, name);
    if (!db) return false;
    inst->current = db;
    return true;
}
