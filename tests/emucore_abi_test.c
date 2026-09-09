#include "libcrankemu.h"
#include "pdll.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*event_handler_fn)(PlaydateAPI *, PDSystemEvent, uint32_t);

static void fail(const char *message) {
  fprintf(stderr, "emucore ABI check failed: %s\n", message);
  exit(1);
}

static void *required_symbol(pdll_t *loader, const char *name) {
  void *symbol = loader->getSymbol ? loader->getSymbol(name) : NULL;
  if (!symbol) {
    fprintf(stderr, "emucore ABI check failed: missing %s\n", name);
    exit(1);
  }
  return symbol;
}

int main(int argc, char **argv) {
  if (argc != 2)
    fail("usage: emucore_abi_test <core.dylib>");

  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library)
    fail(dlerror());

  event_handler_fn event_handler =
      (event_handler_fn)dlsym(library, "eventHandler");
  if (!event_handler)
    fail("eventHandler is not visible to the dynamic loader");

  PlaydateAPI playdate = {0};
  pdll_t loader = {0};
  loader.playdate_ptr = &playdate;
  event_handler((PlaydateAPI *)(void *)&loader, kEventInit,
                PDLL_DYNAMIC_INIT_ARG);
  if (!loader.getSymbol)
    fail("pdll handshake did not install getSymbol");

  const char *(*core_id)(void) = required_symbol(&loader, "ce_core_id");
  const char *(*core_name)(void) = required_symbol(&loader, "ce_core_name");
  const char *(*core_version)(void) =
      required_symbol(&loader, "ce_core_version");
  const char *(*system_slugs)(void) =
      required_symbol(&loader, "ce_get_system_slugs");
  uint32_t (*get_version)(void) = required_symbol(&loader, "ce_get_version");
  const char *(*rom_info)(const uint8_t *, size_t) =
      required_symbol(&loader, "get_rom_info");
  size_t (*rom_save_size)(const uint8_t *, size_t) =
      required_symbol(&loader, "ce_get_rom_save_size");

  static const char *required[] = {
      "ce_set_frontend",    "ce_load_rom",      "ce_unload_rom",
      "ce_play",            "ce_stop",          "ce_update",
      "ce_full_redraw",     "ce_get_save_size", "ce_is_save_dirty",
      "ce_save",            "ce_load",          "ce_get_preferences",
      NULL,
  };
  for (const char **name = required; *name; ++name)
    required_symbol(&loader, *name);

  uint8_t header[16] = {'N', 'E', 'S', 0x1a, 2, 1, 0x12, 0};
  if (get_version() != CRANKEMU_VERSION)
    fail("unexpected libcrankemu version");
  if (strcmp(core_id(), "nofrendo") || strcmp(core_version(), "0.4") ||
      strcmp(system_slugs(), "nes"))
    fail("unexpected core metadata");
  if (!core_name() || !strstr(core_name(), "FamiCrank"))
    fail("core name is missing");
  if (!strstr(rom_info(header, sizeof(header)), "Mapper:\t1"))
    fail("iNES metadata parser returned the wrong mapper");
  if (rom_save_size(header, sizeof(header)) != 8192)
    fail("battery-backed iNES ROM did not advertise 8KB save RAM");
  header[6] &= (uint8_t)~0x02;
  if (rom_save_size(header, sizeof(header)) != 0)
    fail("non-battery iNES ROM advertised save RAM");

  printf("emucore ABI check passed: %s %s (%s)\n", core_name(),
         core_version(), system_slugs());
  event_handler(&playdate, kEventTerminate, 0);
  dlclose(library);
  return 0;
}
