#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TERM_DEFAULT 0x01000000u /* color flag: use theme default */

#define ATTR_BOLD 1
#define ATTR_UL   2
#define ATTR_REV  4

#define TERM_MAXP    16
#define TERM_MAX_IMG 64

typedef struct {
    uint32_t ch;   /* unicode codepoint */
    uint32_t fg;   /* 0xRRGGBB or TERM_DEFAULT */
    uint32_t bg;
    uint8_t  attr;
} Cell;

/* inline image anchored to screen rows; scrolls with the text */
typedef struct {
    int       id;
    int       row, col;   /* top-left cell (row may be negative = partly scrolled off) */
    int       cols, rows; /* size in cells */
    int       w, h;       /* size in pixels */
    uint32_t *px;         /* w*h, 0x00RRGGBB, already composited on the theme bg */
} Placement;

typedef struct {
    int       w, h;
    uint32_t *px;
} FocusImage;

typedef struct {
    int   cols, rows;
    Cell *main_buf, *alt_buf, *cells; /* cells -> active buffer */
    bool  alt;

    int   cx, cy;
    bool  wrap;                 /* pending wrap */
    int   top, bot;             /* scroll region (inclusive) */
    uint32_t fg, bg;            /* current pen */
    uint32_t ansi[16];          /* ANSI color palette */
    uint8_t  attr;
    int   scx, scy;             /* saved cursor */
    uint32_t sfg, sbg;
    uint8_t  sattr;

    bool  cursor_visible, app_cursor, autowrap, bracketed;

    /* parser */
    int   state;
    int   params[TERM_MAXP];
    int   np;
    char  priv, inter;
    uint32_t utf_cp;
    int   utf_need;
    char  osc[4096];
    int   osc_len;
    char  title[128];

    /* images; frontend must set cell_w/cell_h/bg_rgb before term_spawn */
    int       cell_w, cell_h;
    uint32_t  bg_rgb;
    Placement img[TERM_MAX_IMG];
    int       nimg, next_id;
    bool      image_focus;
    FocusImage focus_img[TERM_MAX_IMG];
    int       nfocus, focus_index;
    uint64_t  focus_serial;
    char      bindir[256];      /* holds the generated `.image` command */
    char      video_path[4096];
    uint64_t  video_serial;
    int       video_row, video_col;
    int       video_max_w, video_max_h;
    int       video_rows;
    bool      video_cancelled;

    int   fd;
    pid_t pid;
    bool  alive;
} Term;

void term_init(Term *t, int cols, int rows);
bool term_spawn(Term *t);
void term_free(Term *t);
void term_resize(Term *t, int cols, int rows);
void term_close_image_focus(Term *t);
bool term_pump(Term *t);                          /* read pty, feed parser; true if data */
void term_feed(Term *t, const char *s, size_t n); /* feed bytes to parser */
void term_write(Term *t, const char *s, size_t n);/* send bytes to child */

#endif
