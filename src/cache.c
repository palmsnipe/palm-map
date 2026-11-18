// SPDX-License-Identifier: GPL-3.0-or-later
#include "map.h"
#include "memory.h"
#include <stddef.h>

#define CACHE_NAME "Map Tile Cache"
#define CACHE_TYPE 'MapC'
#define CACHE_LIMIT (3UL*1024*1024)
#define RECORD_LIMIT 192
#define MAGIC 0x4d617032UL

typedef struct {
    UInt32 magic, source, created, length;
    CacheMeta meta;
    UInt32 x,y;
    UInt8 z,part;
} DiskHeader;

Boolean SameTile(TileKey a, TileKey b)
{
    return a.source==b.source && a.x==b.x && a.y==b.y && a.z==b.z;
}

void CacheOpen(TileCache *c)
{
    c->db=DmOpenDatabaseByTypeCreator(CACHE_TYPE,CREATOR,dmModeReadWrite);
    if (!c->db && DmCreateDatabase(0,CACHE_NAME,CREATOR,CACHE_TYPE,false)==errNone)
        c->db=DmOpenDatabaseByTypeCreator(CACHE_TYPE,CREATOR,dmModeReadWrite);
}

static Boolean Matches(const DiskHeader *h, TileKey k)
{
    return h->magic==MAGIC && h->source==k.source && h->x==k.x && h->y==k.y && h->z==k.z;
}

UInt8 *CacheRead(TileCache *c, TileKey k, UInt32 *size, CacheMeta *meta)
{
    if (!c->db) return NULL;
    UInt16 count=DmNumRecords(c->db);
    for (UInt16 i=0;i<count;++i) {
        MemHandle h=DmQueryRecord(c->db,i);
        if (!h || MemHandleSize(h)<sizeof(DiskHeader)) continue;
        DiskHeader *head=MemHandleLock(h);
        if (!head) continue;
        DiskHeader first=*head;
        MemHandleUnlock(h);
        if (!Matches(&first,k) || first.part || !first.length || first.length>256UL*1024)
            continue;
        UInt8 *pixels=MapAlloc(first.length);
        if (!pixels) return NULL;
        UInt32 offset=0;
        for (UInt16 j=i;j<count && offset<first.length;++j) {
            UInt32 partSize=first.length-offset;
            if (partSize>BODY_CHUNK) partSize=BODY_CHUNK;
            h=DmQueryRecord(c->db,j);
            if (!h || MemHandleSize(h)!=sizeof(DiskHeader)+partSize) break;
            head=MemHandleLock(h);
            if (!head) break;
            Boolean ok=Matches(head,k) && head->part==j-i &&
                head->created==first.created && head->length==first.length;
            if (ok) MemMove(pixels+offset,head+1,partSize);
            MemHandleUnlock(h);
            if (!ok) break;
            offset+=partSize;
        }
        if (offset==first.length) {
            *size=offset; *meta=first.meta;
            meta->validator[sizeof(meta->validator)-1]=0;
            return pixels;
        }
        MemPtrFree(pixels);
    }
    return NULL;
}

static void MakeSpace(TileCache *c, TileKey k, UInt32 need)
{
    UInt32 total=0;
    UInt16 count=DmNumRecords(c->db);
    for (UInt16 i=count;i>0;) {
        --i;
        MemHandle h=DmQueryRecord(c->db,i);
        if (!h) continue;
        UInt32 n=MemHandleSize(h);
        if (n<sizeof(DiskHeader)) continue;
        DiskHeader *head=MemHandleLock(h);
        if (!head) continue;
        Boolean same=Matches(head,k);
        MemHandleUnlock(h);
        if (same && DmRemoveRecord(c->db,i)==errNone) continue;
        total+=n;
    }
    count=DmNumRecords(c->db);
    while (count && (total+need>CACHE_LIMIT || count+5>RECORD_LIMIT)) {
        MemHandle h=DmQueryRecord(c->db,count-1);
        UInt32 n=h ? MemHandleSize(h) : 0;
        if (DmRemoveRecord(c->db,--count)!=errNone) break;
        total=total>n ? total-n : 0;
    }
}

void CacheWrite(TileCache *c, TileKey k, const UInt8 *data, UInt32 size, const CacheMeta *meta)
{
    if (!c->db || !meta->expires || !size) return;
    MakeSpace(c,k,size+5*sizeof(DiskHeader));
    DiskHeader head={MAGIC,k.source,TimGetSeconds(),size,*meta,k.x,k.y,k.z,0};
    UInt32 off=0;
    UInt16 allocated=0;
    while (off<size) {
        UInt16 index=allocated;
        UInt32 n=size-off;
        if (n>BODY_CHUNK) n=BODY_CHUNK;
        MemHandle h=DmNewRecord(c->db,&index,sizeof(head)+n);
        if (!h) break;
        ++allocated;
        void *p=MemHandleLock(h);
        Boolean ok=p && DmWrite(p,0,&head,sizeof(head))==errNone &&
            DmWrite(p,sizeof(head),data+off,n)==errNone;
        if (p) MemHandleUnlock(h);
        if (DmReleaseRecord(c->db,index,ok)!=errNone || !ok) break;
        ++head.part;
        off+=n;
    }
    if (off!=size) while (allocated--)
        if (DmRemoveRecord(c->db,0)!=errNone) break;
}


void CacheTouch(TileCache *c, TileKey k, UInt32 expires)
{
    if (!c->db) return;
    for (UInt16 i=DmNumRecords(c->db);i>0;) {
        --i;
        MemHandle h=DmQueryRecord(c->db,i);
        if (!h || MemHandleSize(h)<sizeof(DiskHeader)) continue;
        DiskHeader *p=MemHandleLock(h);
        if (!p) continue;
        Boolean found=Matches(p,k);
        MemHandleUnlock(h);
        if (!found) continue;
        if (!expires) { DmRemoveRecord(c->db,i); continue; }
        h=DmGetRecord(c->db,i);
        if (!h) continue;
        p=MemHandleLock(h);
        Boolean ok=p && DmWrite(p,offsetof(DiskHeader,meta)+offsetof(CacheMeta,expires),
            &expires,sizeof(expires))==errNone;
        if (p) MemHandleUnlock(h);
        DmReleaseRecord(c->db,i,ok);
    }
}
