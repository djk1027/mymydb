#ifndef MYMYDB_STORAGE_H
#define MYMYDB_STORAGE_H

/*
 * Umbrella header for the storage layer. v2.0 splits storage into one source
 * file per object type; this header pulls the pieces together so the rest of
 * the engine can keep including "storage.h".
 *
 *   object.h    - Object header, ObjType, PageId, block/extent constants
 *   value.h     - cell values (INT / TEXT)
 *   pager.h     - raw block device (the data file)
 *   table.h     - Table object: 8KB slotted blocks, extents, row cursor
 *   database.h  - Database object: a logical database owning tables
 *   instance.h  - the process instance: databases + pager + checkpoint/load
 */

#include "object.h"
#include "value.h"
#include "param.h"
#include "pager.h"
#include "table.h"
#include "database.h"
#include "instance.h"

#endif /* MYMYDB_STORAGE_H */
