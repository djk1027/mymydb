#ifndef MYMYDB_PAGER_H
#define MYMYDB_PAGER_H

#include "object.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The pager is the raw block device: a single data file viewed as an array of
 * BLOCK_SIZE blocks. Block 0 is reserved for the instance superblock. Higher
 * blocks are handed out contiguously by pager_alloc (used for both table
 * extents and the catalog), which is what keeps an object's data physically
 * contiguous on disk. Catalog layout and object serialization live in
 * instance.c; the pager itself only knows about fixed-size blocks.
 */
typedef struct {
    FILE    *fp;
    char    *path;
    uint32_t hw_blocks;  /* high-water: number of blocks allocated in the file */
    uint32_t block_size; /* runtime block size (bytes) */
} Pager;

/* Opens (creating if absent) the data file at path with the given block size.
 * hw_blocks starts at 1 (block 0 reserved) for a fresh file; the caller resets
 * hw_blocks/block_size from the superblock when reopening. Returns NULL on I/O
 * error. */
Pager *pager_open(const char *path, uint32_t block_size);
void   pager_close(Pager *p);

/* Reads the first `n` bytes of block 0 (used to recover the block size from an
 * existing file's superblock before the real block size is known). */
bool pager_read_head(Pager *p, void *buf, size_t n);

/* True if path already names a non-empty file (i.e. an existing database). */
bool pager_file_exists(const char *path);

/* Reads/writes one BLOCK_SIZE block at physical block index `block`. */
bool pager_read_block(Pager *p, uint32_t block, void *buf);
bool pager_write_block(Pager *p, uint32_t block, const void *buf);

/* Reserves `n` contiguous blocks and returns the first block's index. */
uint32_t pager_alloc(Pager *p, uint32_t n);

void pager_flush(Pager *p);

#endif /* MYMYDB_PAGER_H */
