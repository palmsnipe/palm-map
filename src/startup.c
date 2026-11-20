#include <PalmOS.h>

UInt32 PilotMain(UInt16 command, void *commandPBP, UInt16 launchFlags);
UInt32 __Startup__(void);

#define LINK_CODE_BASE 0x10000000UL
#define LINK_CODE_LIMIT 0x10010000UL

static void ApplyAbsoluteRelocations(void) {
    UInt32 runtimeBase = (UInt32)&__Startup__;
    UInt32 delta;
    UInt32 tableStart, tableEnd;

    __asm__ volatile ("" : "+d" (runtimeBase));
    delta = runtimeBase - LINK_CODE_BASE;
    if (delta == 0) return;
    if (MemSemaphoreReserve(true) != errNone) return;

    // The table follows the code and may be beyond a 16-bit PC displacement.
    __asm__ ("move.l #__reloc_offsets_start,%0\n\tmove.l #__reloc_offsets_end,%1"
        : "=d" (tableStart), "=d" (tableEnd));
    UInt32 *endP = (UInt32 *)(runtimeBase + tableEnd);
    for (UInt32 *offsetP = (UInt32 *)(runtimeBase + tableStart); offsetP < endP; offsetP++) {
        UInt32 *entryP = (UInt32 *)(runtimeBase + *offsetP);
        UInt32 value = *entryP;
        if (value >= LINK_CODE_BASE && value < LINK_CODE_LIMIT) {
            *entryP = value + delta;
        }
    }

    MemSemaphoreRelease(true);
}

UInt32 __attribute__((section(".vectors"))) __Startup__(void) {
    SysAppInfoPtr appInfoP;
    void *previousGlobalsP;
    void *globalsP;
    UInt32 result;

    ApplyAbsoluteRelocations();
    SysAppStartup(&appInfoP, &previousGlobalsP, &globalsP);
    result = PilotMain(appInfoP->cmd, appInfoP->cmdPBP, appInfoP->launchFlags);
    SysAppExit(appInfoP, previousGlobalsP, globalsP);
    return result;
}
