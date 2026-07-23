#ifndef MYMYDB_PARAM_H
#define MYMYDB_PARAM_H

/*
 * v2.1 parameters. A ParamStore is a small ordered set of (name, value) string
 * pairs. The instance keeps one global store (instance-wide settings such as
 * block_size and data_file); each database keeps its own store, seeded from the
 * global defaults when the database is created. Stores are persisted in the
 * catalog and surfaced in table form via SHOW PARAMETERS.
 */

typedef struct {
    char *name;
    char *value;
} Param;

typedef struct {
    Param *items;
    int n;
    int cap;
} ParamStore;

/* Inserts or updates name -> value (value is copied). */
void param_set(ParamStore *s, const char *name, const char *value);
/* Returns the value for name, or NULL if unset. */
const char *param_get(const ParamStore *s, const char *name);
/* Replaces dst's contents with a copy of src. */
void param_copy(ParamStore *dst, const ParamStore *src);
void param_free(ParamStore *s);

#endif /* MYMYDB_PARAM_H */
