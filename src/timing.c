#include <pd_api.h>
#include <osd.h>

extern PlaydateAPI *pd;

static int timerfreq = 60;

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize) {
    timerfreq = frequency;
    return 0;
}

int osd_nofrendo_ticks(void) {
    return pd->system->getCurrentTimeMilliseconds() / (1000 / timerfreq);
}
