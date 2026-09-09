#ifndef LIBCRANKEMU_H_
#define LIBCRANKEMU_H_

/* libcrankemu v1 ABI, mirrored from CrankBoyHQ/crankboy-app.
 * Kept local so a FamiCrank core can be built without a sibling checkout. */
#define CRANKEMU_VERSION 1

#include <pd_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CE_MODAL_NO_CANCEL 1

#define CE_PREF_LOCKED 1
#define CE_PREF_ALWAYS_GLOBAL 4
#define CE_PREF_ALWAYS_LOCAL 8
#define CE_PREF_REQUIRES_RESTART 16
#define CE_PREF_NONDEFAULT 32

typedef struct
{
    bool itcm_allowed;
    bool turbo;
} ce_frontend_settings_t;

typedef struct ce_frontend
{
    uint32_t version;
    void* (*alloc_dtcm)(size_t size, size_t alignment);
    void (*set_error)(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void (*get_buttons)(PDButtons* o_down, PDButtons* o_pressed, PDButtons* o_released);
    void (*blockingModal)(const char* msg, const char* const* options,
                          unsigned flags, void* ud,
                          void (*cb)(void* ud, int arg));
    int (*get_hardware_revision)(void);
    const ce_frontend_settings_t* (*settings)(void);
} ce_frontend_t;

enum ce_preference_type
{
    CE_PREFERENCE_STANDARD,
    CE_PREFERENCE_CATEGORY,
};

typedef struct ce_preference
{
    void* ud;
    enum ce_preference_type type;
    char* id;
    const char* (*name)(struct ce_preference* self);
    const char* (*description)(struct ce_preference* self);
    const char* const* values;
    unsigned (*get)(struct ce_preference* self);
    bool (*set)(struct ce_preference* self, unsigned value);
    uint32_t (*flags)(struct ce_preference* self);
} ce_preference_t;

static inline uint32_t ce_get_version(void) { return CRANKEMU_VERSION; }

#endif /* LIBCRANKEMU_H_ */
