// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAP_MEMORY_H
#define MAP_MEMORY_H
#include <PalmOS.h>

extern SysAppInfoPtr SysGetAppInfo(SysAppInfoPtr *, SysAppInfoPtr *) SYS_TRAP(sysTrapSysGetAppInfo);

// Palm OS 5's large dynamic-chunk allocation, also used by PalmOSGlue.
static inline MemPtr MapAlloc(UInt32 size)
{
    MemPtr p=MemPtrNew(size);
    if (p || size<0x10000UL) return p;
    SysAppInfoPtr ui, action;
    SysAppInfoPtr app=SysGetAppInfo(&ui,&action);
    return app ? MemChunkNew(0,size,
        (UInt16)(app->memOwnerID | memNewChunkFlagNonMovable | 0x1000)) : NULL;
}
static inline Boolean MapGrow(UInt8 **buffer, UInt32 *capacity, UInt32 bytes, UInt32 keep)
{
    if (*capacity>=bytes) return true;
    // Release buffers whose contents can be discarded before allocating a larger one.
    if (!keep && *buffer) {
        MemPtrFree(*buffer);
        *buffer=NULL;
        *capacity=0;
    }
    UInt8 *next=MapAlloc(bytes);
    if (!next) return false;
    if (keep) {
        MemMove(next,*buffer,keep);
        MemPtrFree(*buffer);
    }
    *buffer=next;
    *capacity=bytes;
    return true;
}
static inline Boolean MapReserve(UInt8 **buffer, UInt32 *capacity, UInt32 bytes)
{
    return MapGrow(buffer,capacity,bytes,0);
}
#endif
