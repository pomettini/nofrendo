#include <pd_api.h>
#include <string.h>
#include <osd.h>

extern PlaydateAPI *pd;

#define SAMPLE_RATE  22050
#define RING_SIZE    4096   /* int16 samples; must be power of 2 */
#define RING_MASK    (RING_SIZE - 1)
#define MAX_FILL     1024   /* max samples generated per update call */

static void (*apu_fill)(void *buf, int len) = NULL;

static int16_t          ring[RING_SIZE];
static volatile unsigned int ring_write = 0;   /* written by game thread only */
static volatile unsigned int ring_read  = 0;   /* written by audio thread only */

static uint32_t last_fill_ms = 0;

/* Playdate audio callback — runs on the audio thread */
static int audio_callback(void *ctx, int16_t *left, int16_t *right, int len) {
    /* Source is 22050 Hz, output is 44100 Hz → upsample 2:1.
       len is the number of 44100-Hz samples wanted; we need len/2 source samples. */
    int src_needed = len / 2;
    int available  = (ring_write - ring_read) & RING_MASK;
    int src_count  = available < src_needed ? available : src_needed;

    for (int i = 0; i < src_count; i++) {
        int16_t s = ring[ring_read & RING_MASK];
        ring_read = (ring_read + 1) & RING_MASK;
        left[i * 2]     = s;
        left[i * 2 + 1] = s;
    }

    /* Silence for underrun */
    if (src_count * 2 < len)
        memset(left + src_count * 2, 0, (len - src_count * 2) * sizeof(int16_t));

    return 1;
}

void osd_setsound(void (*playfunc)(void *buffer, int length)) {
    apu_fill     = playfunc;
    last_fill_ms = pd->system->getCurrentTimeMilliseconds();
    pd->sound->addSource(audio_callback, NULL, 0);  /* 0 = mono */
}

void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = SAMPLE_RATE;
    info->bps         = 16;
}

/* Called from the game update callback to keep the ring buffer topped up */
void sound_fill_buffer(void) {
    if (!apu_fill) return;

    uint32_t now_ms  = pd->system->getCurrentTimeMilliseconds();
    uint32_t elapsed = now_ms - last_fill_ms;
    last_fill_ms     = now_ms;

    int samples = (int)((elapsed * SAMPLE_RATE) / 1000);
    int space   = (ring_read - ring_write - 1 + RING_SIZE) & RING_MASK;
    if (samples > space)    samples = space;
    if (samples > MAX_FILL) samples = MAX_FILL;
    if (samples <= 0) return;

    int16_t tmp[MAX_FILL];
    apu_fill(tmp, samples);

    for (int i = 0; i < samples; i++) {
        ring[ring_write & RING_MASK] = tmp[i];
        ring_write = (ring_write + 1) & RING_MASK;
    }
}
