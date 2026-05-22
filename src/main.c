#include <pd_api.h>
#include <nofrendo.h>
#include "nofrendo/cpu/nes6502.h"

PlaydateAPI *pd;

extern void nes6502_itcm_init(void *(*alloc_fn)(void *, size_t));

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
    if (event == kEventInit) {
        pd = playdate;
        pd->display->setRefreshRate(60.0f);
        nes6502_itcm_init(pd->system->realloc);   /* copy execute loop to ITCM before any NES init */
        nofrendo_main(0, NULL);
    }
    return 0;
}
