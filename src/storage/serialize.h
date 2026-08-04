#ifndef MYMYDB_SERIALIZE_H
#define MYMYDB_SERIALIZE_H

#include "param.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Little growable byte buffer + bounds-checked reader used to (de)serialize the
 * master registry and per-database catalogs. Shared by instance.c and dbfile.c.
 */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

void buf_u32(Buf *b, uint32_t v);
void buf_u16(Buf *b, uint16_t v);
void buf_u8(Buf *b, uint8_t v);
void buf_str(Buf *b, const char *s);
void buf_params(Buf *b, const ParamStore *s);
void buf_free(Buf *b);

typedef struct { const uint8_t *data; size_t len, pos; } Rdr;

uint32_t rd_u32(Rdr *r);
uint16_t rd_u16(Rdr *r);
uint8_t  rd_u8(Rdr *r);
void     rd_str(Rdr *r, char *dst, int cap);
char    *rd_strdup(Rdr *r);
void     rd_params(Rdr *r, ParamStore *s);

#endif /* MYMYDB_SERIALIZE_H */
