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

#ifdef PD_PLAYBENCH_RECORD_KIRBY
static void rec_launch_rom(const char *path, void *userdata);
#endif

static void start_rom_picker(void) {
  pd->system->removeAllMenuItems();
  clear_menu_handles();
  install_settings_menu_items();

  static const char *extensions[] = {"nes", NULL};
  RomPickerConfig config = {
      .folder = ROM_PICKER_FOLDER,
      .extensions = extensions,
#ifdef PD_PLAYBENCH_RECORD_KIRBY
      .on_select = rec_launch_rom,
#else
      .on_select = launch_rom,
#endif
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
static char bench_incompatible_rom_path[ROM_PICKER_MAX_PATH];
static int bench_rom_found;
static int bench_incompatible_rom_found;

static int bench_name_contains(const char *s, const char *target) {
  for (; *s; s++) {
    const char *a = s;
    const char *m = target;
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

static int bench_name_has_target(const char *s) {
#ifdef PD_PLAYBENCH_KIRBY
  /* The committed input capture was recorded against the PAL/Europe release.
     The USA Rev 1 title reaches menus on different emulated frames, so accepting
     any Kirby filename silently produces a different workload. */
  return bench_name_contains(s, "kirby") && bench_name_contains(s, "europe");
#else
  /* The Mario capture is a PAL replay too; another region is not an exact
     correctness or performance comparison. */
  return bench_name_contains(s, "mario") && bench_name_contains(s, "europe");
#endif
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
#ifdef PD_PLAYBENCH_KIRBY
  if (!bench_incompatible_rom_found && bench_name_contains(filename, "kirby") &&
      !bench_name_contains(filename, "europe")) {
#else
  if (!bench_incompatible_rom_found && bench_name_contains(filename, "mario") &&
      !bench_name_contains(filename, "europe")) {
#endif
    snprintf(bench_incompatible_rom_path, sizeof(bench_incompatible_rom_path),
             "%s%s", ROM_PICKER_FOLDER, filename);
    bench_incompatible_rom_found = 1;
  }
  if (!bench_rom_found && bench_name_has_target(filename)) {
    snprintf(bench_rom_path, sizeof(bench_rom_path), "%s%s", ROM_PICKER_FOLDER,
             filename);
    bench_rom_found = 1;
  }
}

/* Find only the PAL ROM against which the committed input replay was recorded. */
static const char *bench_find_rom(void) {
  bench_rom_found = 0;
  bench_incompatible_rom_found = 0;
  pd->file->listfiles(ROM_PICKER_FOLDER, bench_scan_file, NULL, 0);
  return bench_rom_found ? bench_rom_path : NULL;
}
#endif /* shared benchmark/record harness */

#ifdef PD_PLAYBENCH_ENABLED
#include "pd_playbench.h"
#endif

#if defined(PD_PLAYBENCH_ENABLED) && !defined(PD_PLAYBENCH_RECORD)
/* Recorded script written by the record build; must match REC_SCRIPT_PATH in
   keyboard.c. Loaded in preference to the built-in script when present. */
#ifdef PD_PLAYBENCH_KIRBY
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/kirby_1_1.txt"
#define BUNDLED_SCRIPT_PATH "nes_kirby_adventure_1_1.txt"
#define BENCH_TEST_NAME "nes_kirby_adventure_1_1"
#define BENCH_ROM_NAME "Kirby's Adventure"
#if defined(NES6502_PAIRPROFILE) && defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-pairprof"
#elif defined(NES6502_ZP_BEQ_SPIN) && defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto-linked-zpspin"
#elif defined(NES6502_CHAIN_F0_A5) && defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto-linked-f0a5"
#elif defined(NES6502_FUSE_INCDEC_BNE) && defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto-linked-fuse"
#elif defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO) && defined(PPU_BG_PAIR_FAST) && defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto-linked"
#elif defined(ENABLE_LTO_NO_IPA_CLONE) && defined(PPU_BG_PAIR_FAST) && defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto-noclone"
#elif defined(ENABLE_LTO) && defined(PPU_BG_PAIR_FAST) && defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-lto"
#elif defined(DIAG_CPU_EXEC_TIMING) && defined(PPU_BG_PAIR_FAST) && defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-profile"
#elif defined(PPU_BG_PAIR_FAST) && defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-irqpair"
#elif defined(PPU_BG_PAIR_FAST)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-bgpair"
#elif !defined(NES_IRQ_MAPPER_BATCH)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-noirqbatch"
#elif defined(NES_IRQ_MAPPER_BATCH_IRQ_SCOPE)
#define BENCH_BUILD_LABEL "0.4-bench-kirby-irqonly"
#else
#define BENCH_BUILD_LABEL "0.4-bench-kirby-base"
#endif
#else
#define REC_SCRIPT_PATH "/Shared/Emulation/nes/rec_script.txt"
#define BUNDLED_SCRIPT_PATH "nes_smb1_world_1_1.txt"
#define BENCH_TEST_NAME "nes_smb1_world_1_1"
#define BENCH_ROM_NAME "Super Mario Bros"
#if defined(NES6502_ZP_BEQ_SPIN) && defined(NES6502_LINKED_CORE) && defined(ENABLE_LTO)
#define BENCH_BUILD_LABEL "0.4-bench-prod-zpspin"
#elif defined(PPU_BG_PAIR_FAST)
#define BENCH_BUILD_LABEL "0.4-bench-bgpair"
#else
#define BENCH_BUILD_LABEL "0.4-bench"
#endif
#endif

/* Minimal fallback, used only if neither the /Shared override nor the bundled
   script is present: boot the game and idle briefly, then stop. The real
   benchmark is the committed, bundled nes_smb1_world_1_1.txt recording. */
#ifndef PD_PLAYBENCH_KIRBY
static const char *bench_build_script(void) {
  return "wait 200\n"   /* title screen */
         "hold UP 10\n" /* press Start   */
         "wait 600\n"   /* smoke-test idle */
         "stop\n";
}
#endif

static void bench_run(void) {
  const char *path = bench_find_rom();
  if (!path) {
    if (bench_incompatible_rom_found) {
      pd->system->logToConsole(
          "[bench] incompatible ROM %s; replay requires Europe release",
          bench_incompatible_rom_path);
      start_rom_picker();
      return;
    }
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
      .test_name = BENCH_TEST_NAME,
      .emulator_name = "nofrendo",
      .rom_name = BENCH_ROM_NAME,
      .build_label = BENCH_BUILD_LABEL,
      .device_label = "device",
      .report_path = "benchmarks/latest.txt",
      .target_fps = 50,
      .log_to_console = 1,
      .write_report_file = 1,
      .input_mode = PD_PLAYBENCH_INPUT_OVERRIDE,
  };
  pd_playbench_init(pd, &cfg);
  /* Load order: the matching /Shared recording, then the committed script
     bundled in the .pdx. Mario alone retains its minimal smoke fallback. */
  if (pd_playbench_load_script_from_file(REC_SCRIPT_PATH)) {
    pd->system->logToConsole("[bench] loaded override %s", REC_SCRIPT_PATH);
  } else if (pd_playbench_load_script_from_file(BUNDLED_SCRIPT_PATH)) {
    pd->system->logToConsole("[bench] loaded bundled script");
#ifndef PD_PLAYBENCH_KIRBY
  } else if (!pd_playbench_load_script_from_string(bench_build_script())) {
    pd->system->logToConsole("[bench] script error: %s",
                             pd_playbench_get_last_error());
    return;
#else
  } else {
    pd->system->logToConsole("[bench] missing Kirby script: %s",
                             pd_playbench_get_last_error());
    return;
#endif
  }
#ifdef NES6502_PAIRPROFILE
  nes6502_pair_profile_reset();
#endif
  pd_playbench_start();
  pd->system->logToConsole("[bench] script started: %s", cfg.test_name);
}
#endif /* replay only */

#ifdef PD_PLAYBENCH_RECORD
extern void osd_rec_dump(void); /* implemented in keyboard.c */

static void rec_dump_menu(void *ud) {
  (void)ud;
  osd_rec_dump();
}

/* Launch one ROM and begin recording the effective buttons each emulated frame.
   "Dump script" in the system menu saves the replayable script. */
static void rec_start_rom(const char *path) {
  pd->system->logToConsole(
      "[rec] recording %s -- play it, then pick 'Dump script' from the menu",
      path);
  launch_rom(path, NULL);
  if (!emulator_started) {
    pd->system->logToConsole("[rec] ROM failed to load; recorder aborted");
    return;
  }

  pd_playbench_init(pd, NULL); /* sets the file API for record_save */
  pd_playbench_record_start();

  pd->system->removeAllMenuItems();
  clear_menu_handles();
  pd->system->addMenuItem("Dump script", rec_dump_menu, NULL);
}

#ifdef PD_PLAYBENCH_RECORD_KIRBY
static void rec_launch_rom(const char *path, void *userdata) {
  (void)userdata;
  rec_start_rom(path);
}

static void rec_run(void) {
  pd->system->logToConsole(
      "[rec] Kirby recorder ready -- choose the Kirby ROM from the picker");
  start_rom_picker();
}
#else
/* The original Mario recorder retains its auto-load workflow. */
static void rec_run(void) {
  const char *path = bench_find_rom();
  if (!path) {
    pd->system->logToConsole("[rec] no .nes ROM in %s; opening picker",
                             ROM_PICKER_FOLDER);
    start_rom_picker();
    return;
  }
  rec_start_rom(path);
}
#endif
#endif /* PD_PLAYBENCH_RECORD */

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
  if (event == kEventInit) {
    pd = playdate;
    pd->display->setRefreshRate(60.0f);
    osd_load_settings(); /* restore saved Frameskip / Show FPS preferences */
#ifndef NES6502_LINKED_CORE
    nes6502_itcm_init(
        pd->system
            ->realloc); /* copy execute loop to ITCM before any NES init */
#endif
#ifdef NES6502_TCMHOT_CORE
    nes6502_tcmhot_core_init(pd->system->clearICache);
    log_tcmhot_core_status();
#endif
#ifdef NES6502_TCMHOT_PROBE
    run_tcmhot_probe();
#endif
#if defined(PD_PLAYBENCH_RECORD)
    rec_run();   /* start the selected live-recording workflow */
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
