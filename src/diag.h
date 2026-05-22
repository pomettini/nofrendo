#pragma once

#ifdef DIAG
#include <stdbool.h>
void diag_frame_begin(void);
void diag_frame_end(void);
void diag_render_begin(bool draw_flag);
void diag_render_end(void);
#else
#define diag_frame_begin()         ((void)0)
#define diag_frame_end()           ((void)0)
#define diag_render_begin(draw)    ((void)0)
#define diag_render_end()          ((void)0)
#endif
