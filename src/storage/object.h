#ifndef MYMYDB_OBJECT_H
#define MYMYDB_OBJECT_H

#include <stdint.h>

/*
 * v2.0 object model. Everything in the engine is an "object" identified by an
 * id and a type. Two object types exist so far: a logical database and a table.
 * Each object type has its own struct (see database.h / table.h) that embeds an
 * Object header.
 */

#define MAX_NAME 64

/*
 * v2.0: the on-disk allocation unit is a Block (was a 4KB page). v2.1 makes the
 * block size a runtime parameter (global, fixed when a data file is created);
 * 8KB is the default. Bounds keep buffers and slot offsets (u16) sane.
 */
#define DEFAULT_BLOCK_SIZE 8192
#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE 65536

/* Blocks are handed out in extents; the first extent is 8 blocks and each
 * subsequent extent doubles (8, 16, 32, ...). Extent i therefore holds
 * EXTENT_INIT_BLOCKS << i blocks. Contiguous physical placement of an extent
 * keeps an object's blocks LBA-contiguous for sequential scans. */
#define EXTENT_INIT_BLOCKS 8

typedef enum {
    OBJ_DB,     /* a logical database */
    OBJ_TABLE,  /* a table */
} ObjType;

/* Common header embedded at the top of every object struct. */
typedef struct {
    uint32_t id;
    ObjType  type;
} Object;

/*
 * A globally-unique block identifier: (db id, object id, sequence). The
 * sequence is the block's ordinal within its owning object. Since (db_id,
 * obj_id) is unique per object and seq is unique within an object, no two live
 * blocks ever share a PageId.
 */
typedef struct {
    uint32_t db_id;
    uint32_t obj_id;
    uint32_t seq;
} PageId;

const char *objtype_name(ObjType t);

#endif /* MYMYDB_OBJECT_H */
