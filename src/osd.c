#include <pd_api.h>
#include <string.h>
#include <noftypes.h>
#include <nofconfig.h>
#include <osd.h>
#include <nofrendo.h>
#include <nes.h>

extern PlaydateAPI *pd;

static char configfilename[] = "nofrendo.cfg";

static int playdate_update(void *ud) {
    nes_renderframe(true);
    osd_getinput();
    return 1;
}

void osd_start_emulation(void) {
    pd->system->setUpdateCallback(playdate_update, NULL);
}

int osd_init(void) {
    return 0;
}

void osd_shutdown(void) {
}

int osd_main(int argc, char *argv[]) {
    config.filename = configfilename;
    return main_loop("cartridge.nes", system_nes);
}

void osd_getmouse(int *x, int *y, int *button) {
}

void osd_fullname(char *fullname, const char *shortname) {
    strncpy(fullname, shortname, PATH_MAX);
}

char *osd_newextension(char *string, char *ext) {
    int l = strlen(string);
    while (l && string[l] != '.')
        l--;
    if (l) string[l] = 0;
    strcat(string, ext);
    return string;
}

int osd_makesnapname(char *filename, int len) {
    return -1;
}

static char *rom_data = NULL;

char *osd_getromdata(const char *name) {
    FileStat stat;
    if (pd->file->stat(name, &stat) != 0)
        return NULL;

    rom_data = malloc(stat.size);
    if (!rom_data)
        return NULL;

    SDFile *f = pd->file->open(name, kFileRead);
    if (!f) {
        free(rom_data);
        rom_data = NULL;
        return NULL;
    }

    pd->file->read(f, rom_data, stat.size);
    pd->file->close(f);
    return rom_data;
}

void osd_unloadromdata(void) {
    free(rom_data);
    rom_data = NULL;
}
