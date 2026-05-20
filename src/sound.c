#include <osd.h>

void osd_setsound(void (*playfunc)(void *buffer, int length)) {
}

void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = 22050;
    info->bps = 8;
}
