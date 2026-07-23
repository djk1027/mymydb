#include "pager.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Pager *pager_open(const char *path, uint32_t block_size) {
    FILE *fp = fopen(path, "r+b");     /* existing file */
    if (!fp) fp = fopen(path, "w+b");  /* or create a new one */
    if (!fp) return NULL;

    Pager *p = calloc(1, sizeof(Pager));
    p->fp = fp;
    p->path = strdup(path);
    p->hw_blocks = 1; /* block 0 is the superblock; caller may override */
    p->block_size = block_size;
    return p;
}

bool pager_read_head(Pager *p, void *buf, size_t n) {
    if (fseek(p->fp, 0, SEEK_SET) != 0) return false;
    size_t got = fread(buf, 1, n, p->fp);
    if (got < n) memset((uint8_t *)buf + got, 0, n - got);
    return true;
}

void pager_close(Pager *p) {
    if (!p) return;
    if (p->fp) fclose(p->fp);
    free(p->path);
    free(p);
}

bool pager_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

bool pager_read_block(Pager *p, uint32_t block, void *buf) {
    if (fseek(p->fp, (long)block * p->block_size, SEEK_SET) != 0) return false;
    size_t n = fread(buf, 1, p->block_size, p->fp);
    if (n < p->block_size) /* short read past EOF: treat the rest as zeros */
        memset((uint8_t *)buf + n, 0, p->block_size - n);
    return true;
}

bool pager_write_block(Pager *p, uint32_t block, const void *buf) {
    if (fseek(p->fp, (long)block * p->block_size, SEEK_SET) != 0) return false;
    return fwrite(buf, 1, p->block_size, p->fp) == p->block_size;
}

uint32_t pager_alloc(Pager *p, uint32_t n) {
    uint32_t start = p->hw_blocks;
    p->hw_blocks += n;
    return start;
}

void pager_flush(Pager *p) {
    if (p && p->fp) fflush(p->fp);
}
