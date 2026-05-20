#include <pd_api.h>
#include <nofrendo.h>

PlaydateAPI *pd;

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
    if (event == kEventInit) {
        pd = playdate;
        pd->display->setRefreshRate(60.0f);
        nofrendo_main(0, NULL);
    }
    return 0;
}
