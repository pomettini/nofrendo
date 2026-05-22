#pragma once

#ifdef DIAG
#ifndef _TYPES_H_
#include <stdbool.h>
#endif
void diag_frame_begin(void);
void diag_frame_end(void);
void diag_render_begin(bool draw_flag);
void diag_render_end(void);
bool diag_ppu_bg_enabled(void);
bool diag_ppu_sprites_enabled(void);
void diag_set_ppu_bg_enabled(bool enabled);
void diag_set_ppu_sprites_enabled(bool enabled);
#else
#define diag_frame_begin()         ((void)0)
#define diag_frame_end()           ((void)0)
#define diag_render_begin(draw)    ((void)0)
#define diag_render_end()          ((void)0)
#define diag_ppu_bg_enabled()      1
#define diag_ppu_sprites_enabled() 1
#endif
