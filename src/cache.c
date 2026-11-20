// SPDX-License-Identifier: GPL-3.0-or-later
#include "map.h"
#include "memory.h"
#include <stddef.h>

#define CACHE_NAME "Map Tile Cache"
#define CACHE_TYPE 'MapC'
#define CACHE_LIMIT (3UL*1024*1024)
#define RECORD_LIMIT 192
#define MAGIC 0x4d617032UL
#define LAST_BODY_PART ((MAP_BODY_MAX-1)/BODY_CHUNK)

_Static_assert(LAST_BODY_PART<=255, "tile part number must fit in a byte");

typedef struct {
    UInt32 magic, source, created, length;
    CacheMeta meta;
    UInt32 x,y;
    UInt8 z,part;
} DiskHeader;

typedef struct CacheEntry {
    TileKey key;
    UInt16 record;
} CacheEntry;

Boolean SameTile(TileKey a, TileKey b)
{
    return a.source==b.source && a.x==b.x && a.y==b.y && a.z==b.z;
}

static TileKey HeaderKey(const DiskHeader *h) { return (TileKey){h->source,h->x,h->y,h->z}; }

static Boolean Matches(const DiskHeader *h, TileKey k)
{
    return h->magic==MAGIC && SameTile(HeaderKey(h),k);
}

static Boolean ReadHeader(MemHandle h, DiskHeader *header)
{
    if (!h || MemHandleSize(h)<sizeof(*header)) return false;
    const DiskHeader *p=MemHandleLock(h);
    if (!p) return false;
    *header=*p;
    MemHandleUnlock(h);
    return header->magic==MAGIC;
}

void CacheOpen(TileCache *c)
{
    c->db=DmOpenDatabaseByTypeCreator(CACHE_TYPE,CREATOR,dmModeReadWrite);
    if (!c->db && DmCreateDatabase(0,CACHE_NAME,CREATOR,CACHE_TYPE,false)==errNone)
        c->db=DmOpenDatabaseByTypeCreator(CACHE_TYPE,CREATOR,dmModeReadWrite);
    if (c->db) c->entries=MemPtrNew(RECORD_LIMIT*sizeof(*c->entries));
    c->dirty=true;
}

void CacheClose(TileCache *c)
{
    if (c->db) DmCloseDatabase(c->db);
    if (c->entries) MemPtrFree(c->entries);
}

static void BuildDirectory(TileCache *c)
{
    UInt16 records=DmNumRecords(c->db);
    c->dirty=false;
    c->indexed=c->entries && records<=RECORD_LIMIT;
    c->count=0;
    if (!c->indexed) return;
    for (UInt16 i=0;i<records;++i) {
        DiskHeader h;
        if (ReadHeader(DmQueryRecord(c->db,i),&h) && !h.part)
            c->entries[c->count++]=(CacheEntry){HeaderKey(&h),i};
    }
}

Boolean CacheRead(TileCache *c, TileKey k, UInt8 **buffer, UInt32 *capacity,
    UInt32 *size, CacheMeta *meta)
{
    if (!c->db) return false;
    if (c->dirty) BuildDirectory(c);
    UInt16 records=DmNumRecords(c->db), entries=c->indexed ? c->count : records;
    for (UInt16 i=0;i<entries;++i) {
        if (c->indexed && !SameTile(c->entries[i].key,k)) continue;
        UInt16 record=c->indexed ? c->entries[i].record : i;
        MemHandle h=DmQueryRecord(c->db,record);
        DiskHeader first;
        if (!ReadHeader(h,&first) || !Matches(&first,k) || first.part ||
            !first.length || first.length>MAP_BODY_MAX) continue;
        if (!MapReserve(buffer,capacity,first.length)) return false;
        UInt32 offset=0;
        for (UInt16 j=record;j<records && offset<first.length;++j) {
            UInt32 partSize=first.length-offset;
            if (partSize>BODY_CHUNK) partSize=BODY_CHUNK;
            h=DmQueryRecord(c->db,j);
            if (!h || MemHandleSize(h)!=sizeof(DiskHeader)+partSize) break;
            const DiskHeader *part=MemHandleLock(h);
            if (!part) break;
            Boolean ok=Matches(part,k) && part->part==j-record &&
                part->created==first.created && part->length==first.length;
            if (ok) MemMove(*buffer+offset,part+1,partSize);
            MemHandleUnlock(h);
            if (!ok) break;
            offset+=partSize;
        }
        if (offset==first.length) {
            *size=offset; *meta=first.meta;
            meta->validator[sizeof(meta->validator)-1]=0;
            return true;
        }
    }
    return false;
}

static void RemoveRange(TileCache *c, UInt16 first, UInt16 end)
{
    while (end>first) {
        if (DmRemoveRecord(c->db,--end)!=errNone) break;
        c->dirty=true;
    }
}

static void Trim(TileCache *c, TileKey k, UInt16 kept)
{
    UInt32 total=0;
    for (UInt16 i=DmNumRecords(c->db);i>0;) {
        MemHandle h=DmQueryRecord(c->db,--i);
        if (!h) continue;
        DiskHeader head;
        if (i>=kept && ReadHeader(h,&head) && Matches(&head,k) &&
            DmRemoveRecord(c->db,i)==errNone) {
            c->dirty=true;
        } else {
            total+=MemHandleSize(h);
        }
    }
    UInt16 count=DmNumRecords(c->db);
    while (count>kept && (count>RECORD_LIMIT || total>CACHE_LIMIT)) {
        UInt16 first=count-1;
        DiskHeader tail, previous;
        if (ReadHeader(DmQueryRecord(c->db,first),&tail) && tail.part<=LAST_BODY_PART) {
            for (UInt8 part=tail.part;part && first>kept;--part) {
                if (!ReadHeader(DmQueryRecord(c->db,first-1),&previous) ||
                    !Matches(&previous,HeaderKey(&tail)) || previous.part!=part-1 ||
                    previous.created!=tail.created || previous.length!=tail.length) break;
                --first;
            }
        }
        // Remove complete old tiles rather than leaving their leading records orphaned.
        for (UInt16 i=count;i>first;) {
            MemHandle h=DmQueryRecord(c->db,--i);
            if (h) total-=MemHandleSize(h);
        }
        RemoveRange(c,first,count);
        UInt16 remaining=DmNumRecords(c->db);
        if (remaining!=first) break;
        count=first;
    }
}

void CacheWrite(TileCache *c, TileKey k, const UInt8 *data, UInt32 size, const CacheMeta *meta)
{
    if (!c->db || !meta->expires || !size || size>MAP_BODY_MAX) return;
    DiskHeader head={MAGIC,k.source,TimGetSeconds(),size,*meta,k.x,k.y,k.z,0};
    UInt32 off=0;
    UInt16 allocated=0;
    // Keep a previous usable copy until all replacement parts have been written.
    while (off<size) {
        UInt16 record=allocated;
        UInt32 n=size-off;
        if (n>BODY_CHUNK) n=BODY_CHUNK;
        MemHandle h=DmNewRecord(c->db,&record,sizeof(head)+n);
        if (!h) break;
        ++allocated;
        c->dirty=true;
        void *p=MemHandleLock(h);
        Boolean ok=p && DmWrite(p,0,&head,sizeof(head))==errNone &&
            DmWrite(p,sizeof(head),data+off,n)==errNone;
        if (p) MemHandleUnlock(h);
        if (DmReleaseRecord(c->db,record,ok)!=errNone || !ok) break;
        ++head.part;
        off+=n;
    }
    if (off==size) Trim(c,k,allocated);
    else RemoveRange(c,0,allocated);
}

void CacheTouch(TileCache *c, TileKey k, UInt32 expires)
{
    if (!c->db) return;
    for (UInt16 i=DmNumRecords(c->db);i>0;) {
        --i;
        DiskHeader head;
        if (!ReadHeader(DmQueryRecord(c->db,i),&head) || !Matches(&head,k)) continue;
        if (!expires) { RemoveRange(c,i,i+1); continue; }
        MemHandle h=DmGetRecord(c->db,i);
        if (!h) continue;
        void *p=MemHandleLock(h);
        Boolean ok=p && DmWrite(p,offsetof(DiskHeader,meta)+offsetof(CacheMeta,expires),
            &expires,sizeof(expires))==errNone;
        if (p) MemHandleUnlock(h);
        DmReleaseRecord(c->db,i,ok);
    }
}
