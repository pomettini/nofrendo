#include "nofrendo/cpu/nes6502.h"
#include <nofrendo.h>
#include <pd_api.h>
#include <rom_picker.h>
#include <stdio.h>
#include <string.h>

PlaydateAPI *pd;

extern void nes6502_itcm_init(void *(*alloc_fn)(void *, size_t));
extern void nes_savesram(void); /* flush battery RAM to its .sav in the saves folder */

#define ROM_PICKER_FOLDER "/Shared/Emulation/nes/games/"

static char selected_rom_path[ROM_PICKER_MAX_PATH];
static int emulator_started = 0;
static int return_to_picker_requested = 0;
static PDMenuItem *frameskip_menu_item = NULL;
static PDMenuItem *show_fps_menu_item = NULL;

static void start_rom_picker(void);
extern int osd_get_frame_skip(void);
extern void osd_set_frame_skip(int skip);
extern int osd_get_show_fps(void);
extern void osd_set_show_fps(int enabled);
extern void osd_save_settings(void);
extern void osd_load_settings(void);
extern const char *osd_get_load_error(void);
extern void osd_clear_load_error(void);

#if defined(NES6502_TCMHOT_PROBE) || defined(NES6502_TCMHOT_CORE)
static void drain_console(void) {
  uint32_t start = pd->system->getCurrentTimeMilliseconds();
  while (pd->system->getCurrentTimeMilliseconds() - start < 150u) {
  }
}
#endif

#ifdef NES6502_TCMHOT_PROBE
static void log_tcmhot_probe_result(const char *name, uint32_t got,
                                    uint32_t expected) {
  pd->system->logToConsole("[tcmhot] %s got=%08x expected=%08x ok=%u",
                           name, got, expected, got == expected);
  drain_console();
}

static void run_tcmhot_probe(void) {
  nes6502_tcmhot_probe_t probe;
  uint32_t result;

  nes6502_tcmhot_probe_init(pd->system->clearICache, &probe);
  pd->system->logToConsole(
      "[tcmhot] status=%u ready=%u size=%u max=%u src=%08x dest=%08x frame=%08x",
      probe.status, probe.ready, probe.size, probe.max_size, probe.source,
      probe.dest, probe.frame);
  drain_console();

  if (!probe.ready)
    return;

  pd->system->logToConsole("[tcmhot] calling entry probe");
  drain_console();
  result = nes6502_tcmhot_probe_call(NES6502_TCMHOT_PROBE_ENTRY_MAGIC);
  log_tcmhot_probe_result("entry", result, NES6502_TCMHOT_PROBE_ENTRY_RESULT);

  pd->system->logToConsole("[tcmhot] calling global probe");
  drain_console();
  result = nes6502_tcmhot_probe_call(NES6502_TCMHOT_PROBE_GLOBAL_MAGIC);
  log_tcmhot_probe_result("global", result, NES6502_TCMHOT_PROBE_GLOBAL_VALUE);

  pd->system->logToConsole("[tcmhot] calling callee probe");
  drain_console();
  result = nes6502_tcmhot_probe_call(NES6502_TCMHOT_PROBE_CALL_MAGIC);
  log_tcmhot_probe_result("call", result, NES6502_TCMHOT_PROBE_CALL_RESULT);
}
#endif

#ifdef NES6502_TCMHOT_CORE
static void log_tcmhot_core_status(void) {
  nes6502_tcmhot_core_status_t status;

  nes6502_tcmhot_core_get_status(&status);
  pd->system->logToConsole(
      "[tcmcore] status=%u ready=%u size=%u max=%u src=%08x dest=%08x frame=%08x",
      status.status, status.ready, status.size, status.max_size, status.source,
      status.dest, status.frame);
  drain_console();
}
#endif

static void clear_screen_to_black(void) {
  pd->graphics->setDrawMode(kDrawModeCopy);
  pd->graphics->clear(kColorBlack);

  uint8_t *frame = pd->graphics->getFrame();
  if (frame)
    memset(frame, 0x00, LCD_ROWSIZE * LCD_ROWS);

  pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
  pd->graphics->display();
}

static void menu_return_to_picker(void *userdata) {
  (void)userdata;
  return_to_picker_requested = 1;
}

static void menu_frameskip_changed(void *userdata) {
  (void)userdata;
  if (frameskip_menu_item) {
    /* Menu index 0 is "Auto" (osd FRAME_SKIP_AUTO = -1); 1..3 map to fixed 0..2 */
    int value = pd->system->getMenuItemValue(frameskip_menu_item);
    osd_set_frame_skip(value - 1);
    osd_save_settings();
  }
}

static void menu_show_fps_changed(void *userdata) {
  (void)userdata;
  if (show_fps_menu_item) {
    osd_set_show_fps(pd->system->getMenuItemValue(show_fps_menu_item));
    osd_save_settings();
  }
}

static void clear_menu_handles(void) {
  frameskip_menu_item = NULL;
  show_fps_menu_item = NULL;
}

static void install_settings_menu_items(void) {
  static const char *frameskip_options[] = {"Auto", "0", "1", "2"};

  clear_menu_handles();

  frameskip_menu_item = pd->system->addOptionsMenuItem(
      "Frameskip", frameskip_options,
      (int)(sizeof(frameskip_options) / sizeof(frameskip_options[0])),
      menu_frameskip_changed, NULL);
  if (frameskip_menu_item)
    pd->system->setMenuItemValue(frameskip_menu_item,
                                 osd_get_frame_skip() + 1);

  show_fps_menu_item = pd->system->addCheckmarkMenuItem(
      "Show FPS", osd_get_show_fps(), menu_show_fps_changed, NULL);
}

static void install_emulator_menu(void) {
  pd->system->removeAllMenuItems();
  clear_menu_handles();

  pd->system->addMenuItem("ROM Picker", menu_return_to_picker, NULL);
  install_settings_menu_items();
}

int app_return_to_picker_if_requested(void) {
  if (!return_to_picker_requested)
    return 0;

  return_to_picker_requested = 0;
  nes_savesram(); /* persist battery RAM before tearing the game down */
  main_quit();
  emulator_started = 0;
  selected_rom_path[0] = '\0';
  start_rom_picker();
  return 1;
}

/* Shown when a ROM fails to load, instead of silently dropping back to the
   picker. Holds the reason reported by the core (e.g. "Mapper 71 not yet
   implemented", "ROM image is truncated") until the user acknowledges it. */
static char load_error_reason[128];
static LCDFont *error_font = NULL;
static LCDFont *error_bold_font = NULL;

static void draw_centered(const char *text, LCDFont *font, int y) {
  if (!font)
    return;
  pd->graphics->setFont(font);
  int w = pd->graphics->getTextWidth(font, text, strlen(text), kUTF8Encoding, 0);
  pd->graphics->drawText(text, strlen(text), kUTF8Encoding, (400 - w) / 2, y);
}

static int error_update(void *userdata) {
  (void)userdata;

  PDButtons pushed;
  pd->system->getButtonState(NULL, &pushed, NULL);

  if (!error_font) {
    const char *err = NULL;
    error_font =
        pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Light.pft", &err);
    error_bold_font =
        pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &err);
  }

  pd->graphics->clear(kColorWhite);
  LCDFont *title_font = error_bold_font ? error_bold_font : error_font;
  draw_centered("Could not load this ROM", title_font, 80);
  if (error_font) {
    pd->graphics->setFont(error_font);
    pd->graphics->drawTextInRect(load_error_reason, strlen(load_error_reason),
                                 kUTF8Encoding, 20, 115, 360, 60, kWrapWord,
                                 kAlignTextCenter);
  }
  draw_centered("Press A to go back", error_font, 190);

  if (pushed & kButtonA)
    start_rom_picker();
  return 1;
}

static void present_load_error(void) {
  const char *reason = osd_get_load_error();
  snprintf(load_error_reason, sizeof(load_error_reason), "%s",
           (reason && reason[0]) ? reason : "The file could not be read.");
  pd->system->setUpdateCallback(error_update, NULL);
}

static void launch_rom(const char *path, void *userdata) {
  (void)userdata;

  if (emulator_started)
    return;

  emulator_started = 1;
  snprintf(selected_rom_path, sizeof(selected_rom_path), "%s", path);
  rom_picker_free();
  clear_screen_to_black();
  install_emulator_menu();

  osd_clear_load_error();
  char *argv[] = {"FamiCrank", selected_rom_path};
  int rc = nofrendo_main(2, argv);
  if (rc != 0) {
    pd->system->logToConsole("[rom] failed to launch %s (%d)",
                             selected_rom_path, rc);
    present_load_error();
    emulator_started = 0;
    selected_rom_path[0] = '\0';
  }
}

static int picker_update(void *userdata) {
  (void)userdata;
  rom_picker_update();
  return 1;
}

static void start_rom_picker(void) {
  pd->system->removeAllMenuItems();
  clear_menu_handles();
  install_settings_menu_items();

  static const char *extensions[] = {"nes", NULL};
  RomPickerConfig config = {
      .folder = ROM_PICKER_FOLDER,
      .extensions = extensions,
      .on_select = launch_rom,
      .userdata = NULL,
      .auto_load_single = 0,
  };

  rom_picker_init(pd, &config);
  pd->system->setUpdateCallback(picker_update, NULL);
}

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
  if (event == kEventInit) {
    pd = playdate;
    pd->display->setRefreshRate(60.0f);
    osd_load_settings(); /* restore saved Frameskip / Show FPS preferences */
    nes6502_itcm_init(
        pd->system
            ->realloc); /* copy execute loop to ITCM before any NES init */
#ifdef NES6502_TCMHOT_CORE
    nes6502_tcmhot_core_init(pd->system->clearICache);
    log_tcmhot_core_status();
#endif
#ifdef NES6502_TCMHOT_PROBE
    run_tcmhot_probe();
#endif
    start_rom_picker();
  } else if (event == kEventPause) {
    /* System menu opened — the realistic "I'm putting it down" moment. */
    if (emulator_started)
      nes_savesram();
  } else if (event == kEventTerminate) {
    if (emulator_started)
      nes_savesram();
  }
  return 0;
}
