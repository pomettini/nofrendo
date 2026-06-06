#include "nofrendo/cpu/nes6502.h"
#include <nofrendo.h>
#include <pd_api.h>
#include <rom_picker.h>
#include <stdio.h>
#include <string.h>

PlaydateAPI *pd;

extern void nes6502_itcm_init(void *(*alloc_fn)(void *, size_t));

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
  if (frameskip_menu_item)
    osd_set_frame_skip(pd->system->getMenuItemValue(frameskip_menu_item));
}

static void menu_show_fps_changed(void *userdata) {
  (void)userdata;
  if (show_fps_menu_item)
    osd_set_show_fps(pd->system->getMenuItemValue(show_fps_menu_item));
}

static void clear_menu_handles(void) {
  frameskip_menu_item = NULL;
  show_fps_menu_item = NULL;
}

static void install_settings_menu_items(void) {
  static const char *frameskip_options[] = {"0", "1", "2"};

  clear_menu_handles();

  frameskip_menu_item = pd->system->addOptionsMenuItem(
      "Frameskip", frameskip_options,
      (int)(sizeof(frameskip_options) / sizeof(frameskip_options[0])),
      menu_frameskip_changed, NULL);
  if (frameskip_menu_item)
    pd->system->setMenuItemValue(frameskip_menu_item, osd_get_frame_skip());

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
  main_quit();
  emulator_started = 0;
  selected_rom_path[0] = '\0';
  start_rom_picker();
  return 1;
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

  char *argv[] = {"FamiCrank", selected_rom_path};
  int rc = nofrendo_main(2, argv);
  if (rc != 0) {
    pd->system->logToConsole("[rom] failed to launch %s (%d)",
                             selected_rom_path, rc);
    emulator_started = 0;
    selected_rom_path[0] = '\0';
    start_rom_picker();
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
    nes6502_itcm_init(
        pd->system
            ->realloc); /* copy execute loop to ITCM before any NES init */
    start_rom_picker();
  }
  return 0;
}
