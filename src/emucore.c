/* CrankBoy/libcrankemu adapter for the FamiCrank Nofrendo core. */
#include "libcrankemu.h"
#include "pdll.h"

#include <nofrendo.h>
#include <nes.h>
#include <nes_rom.h>
#include <pd_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PlaydateAPI *pd = NULL;

extern void nes6502_itcm_init(void *(*alloc_fn)(void *, size_t));
extern int osd_update_frame(void *userdata);
extern int osd_get_frame_skip(void);
extern void osd_set_frame_skip(int skip);
extern void osd_set_external_rom(uint8_t *rom, size_t size);
extern void osd_set_emucore_dtcm_ram(void *ram);
extern void osd_set_frontend_buttons(
    void (*get_buttons)(PDButtons *, PDButtons *, PDButtons *));
extern void vid_force_full_redraw(void);
extern const char *osd_get_load_error(void);
extern void osd_clear_load_error(void);

#define FAMICRANK_NES_RAM_SIZE 0x0800u
#define FAMICRANK_SAVE_SIZE    0x2000u
#define FAMICRANK_REFRESH_RATE 50.0f

void ce_unload_rom(void);
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg);

static const ce_frontend_t *frontend = NULL;
static bool rom_loaded = false;
static bool playing = false;
static bool save_dirty = false;
static size_t save_size = 0;
static uint8_t save_cache[FAMICRANK_SAVE_SIZE];

static bool is_ines(const uint8_t *rom, size_t size) {
  return rom && size >= 16 && rom[0] == 'N' && rom[1] == 'E' &&
         rom[2] == 'S' && rom[3] == 0x1a;
}

static size_t ines_save_size(const uint8_t *rom, size_t size) {
  return is_ines(rom, size) && (rom[6] & 0x02) ? sizeof(save_cache) : 0;
}

static rominfo_t *active_rom(void) {
  nes_t *nes = nes_getcontextptr();
  return rom_loaded && nes ? nes->rominfo : NULL;
}

void ce_set_frontend(const ce_frontend_t *fe) {
  frontend = fe;
  osd_set_frontend_buttons(fe ? fe->get_buttons : NULL);
}

const char *ce_core_id(void) { return "nofrendo"; }
const char *ce_core_name(void) { return "FamiCrank (Nofrendo)"; }
const char *ce_core_version(void) { return "0.4"; }
const char *ce_get_system_slugs(void) { return "nes"; }

const char *ce_get_system_name_from_slug(const char *slug) {
  return slug && strcmp(slug, "nes") == 0 ? "Nintendo Entertainment System"
                                           : NULL;
}

const char *get_rom_header_name(const uint8_t *rom, size_t size) {
  (void)rom;
  (void)size;
  return NULL; /* iNES has no title field. */
}

const char *get_rom_info(const uint8_t *rom, size_t size) {
  static char info[160];
  if (!is_ines(rom, size))
    return "Format:\tUnsupported";

  unsigned mapper = (unsigned)(rom[6] >> 4) | (unsigned)(rom[7] & 0xf0);
  const char *format = (rom[7] & 0x0c) == 0x08 ? "NES 2.0" : "iNES";
  snprintf(info, sizeof(info),
           "System:\tNES\nFormat:\t%s\nMapper:\t%u\nPRG ROM:\t%u KB\nCHR ROM:\t%u KB\nBattery:\t%s",
           format, mapper, (unsigned)rom[4] * 16u, (unsigned)rom[5] * 8u,
           (rom[6] & 0x02) ? "Yes" : "No");
  return info;
}

bool ce_load_rom(uint8_t *rom, size_t size, const char *system_slug,
                 const char *rom_basename) {
  if (!is_ines(rom, size) || !system_slug || strcmp(system_slug, "nes") != 0)
    return false;

  if (rom_loaded)
    ce_unload_rom();

  memset(save_cache, 0, sizeof(save_cache));
  save_size = ines_save_size(rom, size);
  save_dirty = false;
  osd_clear_load_error();
  osd_set_external_rom(rom, size);

#ifdef NES_RAM_DTCM
  /* ce_set_frontend also runs during CrankBoy's metadata scans. Delay this
     non-freeable allocation until a game is actually being loaded. */
  osd_set_emucore_dtcm_ram(NULL);
  if (frontend && frontend->alloc_dtcm) {
    const ce_frontend_settings_t *settings =
        frontend->settings ? frontend->settings() : NULL;
    if (!settings || settings->itcm_allowed)
      osd_set_emucore_dtcm_ram(
          frontend->alloc_dtcm(FAMICRANK_NES_RAM_SIZE, 64));
  }
#endif

  char fallback_name[] = "game.nes";
  char *argv[] = {"FamiCrankCore",
                  (char *)(rom_basename && rom_basename[0] ? rom_basename
                                                           : fallback_name)};
  int rc = nofrendo_main(2, argv);
  if (rc != 0) {
    const char *reason = osd_get_load_error();
    if (frontend && frontend->set_error)
      frontend->set_error("Could not load NES ROM: %s",
                          reason && reason[0] ? reason : "unsupported image");
    osd_set_external_rom(NULL, 0);
    save_size = 0;
    return false;
  }

  rom_loaded = true;
  playing = false;
  vid_force_full_redraw();
  return true;
}

void ce_unload_rom(void) {
  if (rom_loaded)
    main_quit();
  rom_loaded = false;
  playing = false;
  if (pd)
    pd->display->setRefreshRate(0.0f);
  save_dirty = false;
  save_size = 0;
  osd_set_frame_skip(-1);
}

bool ce_play(void) {
  if (!rom_loaded)
    return false;

  const ce_frontend_settings_t *settings =
      frontend && frontend->settings ? frontend->settings() : NULL;
  pd->display->setRefreshRate(settings && settings->turbo
                                  ? 0.0f
                                  : FAMICRANK_REFRESH_RATE);
  playing = true;
  if (save_size)
    save_dirty = true; /* Conservative: Nofrendo has no SRAM write barrier. */
  return true;
}

void ce_stop(void) {
  rominfo_t *rom = active_rom();
  if (rom && save_size && rom->sram)
    memcpy(save_cache, rom->sram, save_size);
  if (rom_loaded)
    main_quit();
  rom_loaded = false;
  playing = false;
  if (pd)
    pd->display->setRefreshRate(0.0f);
}

int ce_update(void) {
  if (!playing || !rom_loaded)
    return 1;

  /* CrankBoy leaves emucore updates uncapped. ce_play sets the same 50 Hz
     cadence used by standalone FamiCrank; advancing more than one frame here
     would make frame-skipped workloads run ahead of real time. */
  osd_update_frame(NULL);
  return 1;
}

void ce_full_redraw(void) {
  vid_force_full_redraw();
  osd_set_frame_skip(osd_get_frame_skip());
}

size_t ce_get_rom_save_size(const uint8_t *rom, size_t size) {
  return ines_save_size(rom, size);
}

/* CrankBoy 2.2.x queries the loaded core through this no-argument spelling. */
size_t ce_get_save_size(void) { return save_size; }

bool ce_is_save_dirty(void) { return save_dirty && save_size != 0; }

void ce_save(uint8_t *buffer, size_t size) {
  if (!buffer || size != save_size || !save_size)
    return;
  rominfo_t *rom = active_rom();
  if (rom && rom->sram)
    memcpy(buffer, rom->sram, save_size);
  else
    memcpy(buffer, save_cache, save_size);
  save_dirty = false;
}

bool ce_load(const uint8_t *buffer, size_t size) {
  if (!buffer || size != save_size || !save_size)
    return false;
  memcpy(save_cache, buffer, save_size);
  rominfo_t *rom = active_rom();
  if (rom && rom->sram)
    memcpy(rom->sram, buffer, save_size);
  save_dirty = false;
  return true;
}

static const char *pref_video_name(ce_preference_t *self) {
  (void)self;
  return "Video";
}
static char pref_video_id[] = "video";
static ce_preference_t pref_video = {
    .type = CE_PREFERENCE_CATEGORY,
    .id = pref_video_id,
    .name = pref_video_name,
};

static const char *pref_frameskip_name(ce_preference_t *self) {
  (void)self;
  return "Frame Skip";
}
static const char *pref_frameskip_description(ce_preference_t *self) {
  (void)self;
  return "Auto keeps gameplay and audio at full speed by reducing visual updates only when needed.";
}
static const char *const pref_frameskip_values[] = {"Auto", "0", "1", "2",
                                                     NULL};
static unsigned pref_frameskip_get(ce_preference_t *self) {
  (void)self;
  return (unsigned)(osd_get_frame_skip() + 1);
}
static bool pref_frameskip_set(ce_preference_t *self, unsigned value) {
  (void)self;
  if (value > 3)
    return false;
  osd_set_frame_skip((int)value - 1);
  return true;
}
static uint32_t pref_frameskip_flags(ce_preference_t *self) {
  (void)self;
  return pref_frameskip_get(self) == 0 ? 0 : CE_PREF_NONDEFAULT;
}
static char pref_frameskip_id[] = "frame_skip";
static ce_preference_t pref_frameskip = {
    .type = CE_PREFERENCE_STANDARD,
    .id = pref_frameskip_id,
    .name = pref_frameskip_name,
    .description = pref_frameskip_description,
    .values = pref_frameskip_values,
    .get = pref_frameskip_get,
    .set = pref_frameskip_set,
    .flags = pref_frameskip_flags,
};

static ce_preference_t *preferences[] = {&pref_video, &pref_frameskip, NULL};
ce_preference_t **ce_get_preferences(void) { return preferences; }

PDLL_EXPORT(ce_get_version, ce_set_frontend, ce_core_id, ce_core_name,
            ce_core_version, ce_get_system_slugs,
            ce_get_system_name_from_slug, ce_load_rom, ce_unload_rom,
            get_rom_header_name, get_rom_info, ce_get_rom_save_size,
            ce_get_save_size, ce_play, ce_stop, ce_update, ce_full_redraw,
            ce_is_save_dirty, ce_save, ce_load, ce_get_preferences,
            eventHandler)

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
  {
    PDLL_EVENT(playdate, event, arg);
  }

  if (event == kEventInit) {
    pd = playdate;
#ifndef NES6502_LINKED_CORE
    nes6502_itcm_init(pd->system->realloc);
#endif
    osd_set_frame_skip(-1);
  } else if (event == kEventTerminate) {
    if (rom_loaded)
      ce_stop();
    osd_set_frontend_buttons(NULL);
    frontend = NULL;
  }
  return 0;
}
