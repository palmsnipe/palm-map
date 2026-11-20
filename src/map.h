// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAP_VIEW_H
#define MAP_VIEW_H
#include <PalmOS.h>

#define CREATOR 'PMap'
#define WORLD ((Int32)0x10000000)
#define BODY_CHUNK 60000UL
#define MAP_BODY_MAX (1024UL * 1024)

typedef struct {
    UInt32 source;
    UInt32 x, y;
    UInt8 z;
} TileKey;

typedef struct {
    UInt32 expires;
    Char validator[128];
    Boolean dateValidator;
} CacheMeta;

typedef struct {
    DmOpenRef db;
    struct CacheEntry *entries;
    UInt16 count;
    Boolean dirty, indexed;
} TileCache;

Boolean SameTile(TileKey a, TileKey b);
void CacheOpen(TileCache *c);
void CacheClose(TileCache *c);
Boolean CacheRead(TileCache *c, TileKey k, UInt8 **buffer, UInt32 *capacity,
    UInt32 *size, CacheMeta *meta);
void CacheWrite(TileCache *c, TileKey k, const UInt8 *data, UInt32 size, const CacheMeta *meta);
void CacheTouch(TileCache *c, TileKey k, UInt32 expires);
static inline UInt32 ReadBE(const UInt8 *p)
{
    return (UInt32)p[0]<<24 | (UInt32)p[1]<<16 | (UInt32)p[2]<<8 | p[3];
}
#endif
