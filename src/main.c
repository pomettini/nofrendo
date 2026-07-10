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

#if defined(PD_PLAYBENCH_ENABLED) || defined(PD_PLAYBENCH_RECORD)
/* Dev-only benchmark/record harness: auto-load the Mario ROM. Never built into
   a release (gated by the PD_PLAYBENCH / PD_PLAYBENCH_RECORD cmake options). */
static char bench_rom_path[ROM_PICKER_MAX_PATH];
static char bench_fallback_path[ROM_PICKER_MAX_PATH];
static int bench_rom_found;
static int bench_fallback_found;

static int bench_name_has_mario(const char *s) {
  for (; *s; s++) {
    const char *a = s;
    const char *m = "mario";
    while (*a && *m) {
      char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
      if (ca != *m)
        break;
      a++;
      m++;
    }
    if (*m == '\0')
      return 1;
  }
  return 0;
}

static int bench_is_nes(const char *name) {
  size_t len = strlen(name);
  if (len < 5 || name[len - 1] == '/')
    return 0;
  const char *e = name + len - 4;
  return e[0] == '.' && (e[1] | 32) == 'n' && (e[2] | 32) == 'e' &&
         (e[3] | 32) == 's';
}

static void bench_scan_file(const char *filename, void *ud) {
  (void)ud;
  if (!bench_is_nes(filename))
    return;
  if (!bench_fallback_found) {
    snprintf(bench_fallback_path, sizeof(bench_fallback_path), "%s%s",
             ROM_PICKER_FOLDER, filename);
    bench_fallback_found = 1;
  }
  if (!bench_rom_found && bench_name_has_mario(filename)) {
    snprintf(bench_rom_path, sizeof(bench_rom_path), "%s%s", ROM_PICKER_FOLDER,
             filename);
    bench_rom_found = 1;
  }
}

/* Find a "mario" ROM in the games folder (else the first .nes). NULL if none. */
static const char *bench_find_rom(void) {
  bench_rom_found = 0;
  bench_fallback_found = 0;
  pd->file->listfiles(ROM_PICKER_FOLDER, bench_scan_file, NULL, 0);
  return bench_rom_found        ? bench_rom_path
         : bench_fallback_found ? bench_fallback_path
                                : NULL;
}
#endif /* shared benchmark/record harness */

#ifdef PD_PLAYBENCH_ENABLED
#include "pd_playbench.h"

/* Recorded script written by the record build; must match REC_SCRIPT_PATH in
   keyboard.c. Loaded in preference to the built-in script when present. */
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/rec_script.txt"

/* Super Mario Bros World 1-1: a real playthrough captured with the record build
   (make bench-record) and replayed deterministically. UP = Start. Regenerate by
   re-recording and pasting the REC lines (prefix stripped) here. */
static const char *bench_build_script(void) {
  return "wait 201\n"
         "hold UP 10\n"
         "wait 149\n"
         "hold RIGHT 3\n"
         "hold RIGHT+B 29\n"
         "hold B 2\n"
         "hold RIGHT+B 4\n"
         "hold B 22\n"
         "wait 12\n"
         "hold RIGHT 12\n"
         "hold RIGHT+B 39\n"
         "hold RIGHT 9\n"
         "hold RIGHT+A 6\n"
         "hold RIGHT 5\n"
         "wait 23\n"
         "hold RIGHT 22\n"
         "hold A 14\n"
         "wait 30\n"
         "hold RIGHT 11\n"
         "wait 24\n"
         "hold RIGHT 37\n"
         "wait 31\n"
         "hold LEFT 5\n"
         "wait 39\n"
         "hold RIGHT 7\n"
         "wait 3\n"
         "hold RIGHT 27\n"
         "hold RIGHT+A 18\n"
         "hold RIGHT 28\n"
         "hold RIGHT+B 8\n"
         "hold RIGHT 20\n"
         "hold RIGHT+A 12\n"
         "hold RIGHT 30\n"
         "hold RIGHT+A 17\n"
         "hold RIGHT 48\n"
         "hold RIGHT+A 31\n"
         "hold RIGHT 19\n"
         "hold RIGHT+A 17\n"
         "hold RIGHT 43\n"
         "hold RIGHT+A 29\n"
         "hold RIGHT 21\n"
         "hold RIGHT+B 7\n"
         "hold RIGHT 17\n"
         "hold RIGHT+B 8\n"
         "hold RIGHT 21\n"
         "hold RIGHT+A 44\n"
         "hold RIGHT 19\n"
         "hold RIGHT+B 13\n"
         "hold RIGHT 7\n"
         "wait 3\n"
         "hold A 5\n"
         "wait 54\n"
         "hold LEFT 8\n"
         "hold LEFT+A 33\n"
         "hold A 1\n"
         "wait 21\n"
         "hold RIGHT 7\n"
         "wait 16\n"
         "hold RIGHT 92\n"
         "hold RIGHT+A 35\n"
         "hold RIGHT 16\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 2\n"
         "wait 2\n"
         "hold B 2\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "wait 4\n"
         "hold B 3\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 1\n"
         "hold B 3\n"
         "wait 3\n"
         "hold RIGHT 1\n"
         "hold RIGHT+B 5\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 5\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 1\n"
         "hold B 3\n"
         "wait 4\n"
         "hold B 5\n"
         "wait 4\n"
         "hold B 5\n"
         "wait 3\n"
         "hold RIGHT 1\n"
         "hold RIGHT+B 4\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 5\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 5\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 6\n"
         "hold RIGHT+B 2\n"
         "hold B 1\n"
         "wait 5\n"
         "hold RIGHT 9\n"
         "hold RIGHT+A 28\n"
         "hold RIGHT 19\n"
         "hold RIGHT+A 12\n"
         "hold RIGHT 6\n"
         "wait 22\n"
         "hold A 3\n"
         "hold RIGHT+A 18\n"
         "hold RIGHT 6\n"
         "hold RIGHT+A 28\n"
         "hold RIGHT 10\n"
         "hold RIGHT+B 13\n"
         "hold RIGHT 34\n"
         "hold RIGHT+A 10\n"
         "hold RIGHT 8\n"
         "wait 15\n"
         "hold A 2\n"
         "hold RIGHT+A 21\n"
         "hold RIGHT 19\n"
         "hold RIGHT+A 19\n"
         "hold RIGHT 32\n"
         "hold RIGHT+B 9\n"
         "hold RIGHT 17\n"
         "hold RIGHT+A 11\n"
         "hold RIGHT 40\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 4\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 3\n"
         "wait 2\n"
         "hold B 3\n"
         "wait 4\n"
         "hold B 3\n"
         "wait 4\n"
         "hold B 4\n"
         "wait 4\n"
         "hold B 1\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 5\n"
         "hold RIGHT+B 3\n"
         "hold RIGHT 6\n"
         "hold RIGHT+B 2\n"
         "wait 11\n"
         "hold A 4\n"
         "hold RIGHT+A 9\n"
         "hold RIGHT 24\n"
         "hold RIGHT+A 20\n"
         "hold RIGHT 20\n"
         "hold RIGHT+A 25\n"
         "hold RIGHT 1\n"
         "wait 19\n"
         "hold A 5\n"
         "hold RIGHT+A 3\n"
         "hold A 1\n"
         "wait 22\n"
         "hold RIGHT 1\n"
         "hold RIGHT+B 23\n"
         "hold RIGHT+A+B 36\n"
         "hold RIGHT+B 38\n"
         "wait 180\n"
         "stop\n";
}

static void bench_run(void) {
  const char *path = bench_find_rom();
  if (!path) {
    pd->system->logToConsole("[bench] no .nes ROM in %s; opening picker",
                             ROM_PICKER_FOLDER);
    start_rom_picker();
    return;
  }

  pd->system->logToConsole("[bench] auto-loading %s", path);
  launch_rom(path, NULL);
  if (!emulator_started) {
    pd->system->logToConsole("[bench] ROM failed to load; benchmark aborted");
    return;
  }

  pd->file->mkdir("benchmarks"); /* report destination must exist */

  PDBenchConfig cfg = {
      .test_name = "nes_smb1_world_1_1",
      .emulator_name = "nofrendo",
      .rom_name = "Super Mario Bros",
      .build_label = "0.3-bench",
      .device_label = "device",
      .report_path = "benchmarks/latest.txt",
      .target_fps = 50,
      .log_to_console = 1,
      .write_report_file = 1,
      .input_mode = PD_PLAYBENCH_INPUT_OVERRIDE,
  };
  pd_playbench_init(pd, &cfg);
  /* Prefer a recording captured with the record build (make bench-record);
     fall back to the built-in script if none is present. */
  if (pd_playbench_load_script_from_file(REC_SCRIPT_PATH)) {
    pd->system->logToConsole("[bench] loaded recorded script %s",
                             REC_SCRIPT_PATH);
  } else if (!pd_playbench_load_script_from_string(bench_build_script())) {
    pd->system->logToConsole("[bench] script error: %s",
                             pd_playbench_get_last_error());
    return;
  }
  pd_playbench_start();
  pd->system->logToConsole("[bench] script started: %s", cfg.test_name);
}
#endif /* PD_PLAYBENCH_ENABLED */

#ifdef PD_PLAYBENCH_RECORD
extern void osd_rec_dump(void); /* implemented in keyboard.c */

static void rec_dump_menu(void *ud) {
  (void)ud;
  osd_rec_dump();
}

/* Auto-load the Mario ROM and let the player play it live; the recorder in
   keyboard.c captures every frame. "Dump script" in the system menu prints the
   replayable pd-playbench script. */
static void rec_run(void) {
  const char *path = bench_find_rom();
  if (!path) {
    pd->system->logToConsole("[rec] no .nes ROM in %s; opening picker",
                             ROM_PICKER_FOLDER);
    start_rom_picker();
    return;
  }

  pd->system->logToConsole(
      "[rec] recording %s -- play it, then pick 'Dump script' from the menu",
      path);
  launch_rom(path, NULL);
  if (!emulator_started) {
    pd->system->logToConsole("[rec] ROM failed to load; recorder aborted");
    return;
  }

  pd->system->removeAllMenuItems();
  clear_menu_handles();
  pd->system->addMenuItem("Dump script", rec_dump_menu, NULL);
}
#endif /* PD_PLAYBENCH_RECORD */

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
#if defined(PD_PLAYBENCH_RECORD)
    rec_run();   /* auto-load Mario for a live recording session */
#elif defined(PD_PLAYBENCH_ENABLED)
    bench_run(); /* auto-load Mario and replay the scripted benchmark */
#else
    start_rom_picker();
#endif
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
