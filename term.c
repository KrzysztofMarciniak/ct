#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "term.h"
#include "image.h"
#include "video.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_ESC_SKIP, S_STR, S_STR_ESC };

#define CELL(t, r, c) ((t)->cells[(r) * (t)->cols + (c)])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---------- colors ---------- */

static const uint32_t ansi16[16] = {
    0x15161e, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7, 0x7dcfff, 0xa9b1d6,
    0x414868, 0xff899d, 0x9fe044, 0xfaba4a, 0x8db0ff, 0xc7a9ff, 0xa4daff, 0xc0caf5,
};

static uint32_t palette(const Term *t, int n) {
    if (n < 0) n = 0;
    if (n < 16) return t->ansi[n];
    if (n < 232) {
        n -= 16;
        int r = n / 36, g = (n / 6) % 6, b = n % 6;
        r = r ? r * 40 + 55 : 0;
        g = g ? g * 40 + 55 : 0;
        b = b ? b * 40 + 55 : 0;
        return (uint32_t)(r << 16 | g << 8 | b);
    }
    if (n > 255) n = 255;
    int v = (n - 232) * 10 + 8;
    return (uint32_t)(v << 16 | v << 8 | v);
}

/* ---------- buffers ---------- */

static Cell blank_cell(const Term *t) {
    Cell c = { ' ', TERM_DEFAULT, t->bg, 0 };
    return c;
}

static void fill(Term *t, Cell *p, int n) {
    Cell b = blank_cell(t);
    for (int i = 0; i < n; i++) p[i] = b;
}

/* ---------- inline images ---------- */

static void drop_image(Term *t, int i) {
    free(t->img[i].px);
    memmove(&t->img[i], &t->img[i + 1], sizeof(Placement) * (size_t)(t->nimg - i - 1));
    t->nimg--;
}

static void clear_images(Term *t) {
    while (t->nimg > 0) drop_image(t, t->nimg - 1);
}

/* move media by d rows (d<0: content scrolled up); drop images fully off-screen */
static void shift_images(Term *t, int d) {
    if (t->alt) return;
    if (t->video_serial) t->video_row += d;
    for (int i = t->nimg - 1; i >= 0; i--) {
        t->img[i].row += d;
        if (t->img[i].row + t->img[i].rows <= 0 || t->img[i].row >= t->rows) drop_image(t, i);
    }
}

/* erase-in-display: mode 0 below/at cursor row, 1 above/at cursor row, 2/3 all */
static void erase_images(Term *t, int mode, int cy) {
    if (t->alt) return;
    if (t->video_serial && !t->video_cancelled) {
        int last = t->video_row + t->video_rows - 1;
        if (mode >= 2 || (mode == 0 && last >= cy) || (mode == 1 && t->video_row <= cy))
            t->video_cancelled = true;
    }
    for (int i = t->nimg - 1; i >= 0; i--) {
        int last = t->img[i].row + t->img[i].rows - 1;
        if (mode >= 2 || (mode == 0 && last >= cy) || (mode == 1 && last <= cy)) drop_image(t, i);
    }
}

static Cell *alloc_buf(int cols, int rows) {
    Cell d = { ' ', TERM_DEFAULT, TERM_DEFAULT, 0 };
    Cell *p = malloc(sizeof(Cell) * (size_t)cols * (size_t)rows);
    if (!p) abort();
    for (int i = 0; i < cols * rows; i++) p[i] = d;
    return p;
}

static void scroll_up(Term *t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    if (top == 0 && bot == t->rows - 1) shift_images(t, -n);
    memmove(&CELL(t, top, 0), &CELL(t, top + n, 0), sizeof(Cell) * (size_t)t->cols * (size_t)(h - n));
    fill(t, &CELL(t, bot - n + 1, 0), t->cols * n);
}

static void scroll_down(Term *t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    if (top == 0 && bot == t->rows - 1) shift_images(t, n);
    memmove(&CELL(t, top + n, 0), &CELL(t, top, 0), sizeof(Cell) * (size_t)t->cols * (size_t)(h - n));
    fill(t, &CELL(t, top, 0), t->cols * n);
}

static void linefeed(Term *t) {
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_index(Term *t) {
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) t->cy--;
}

static void put_char(Term *t, uint32_t cp) {
    if (t->wrap && t->autowrap) {
        t->cx = 0;
        t->wrap = false;
        linefeed(t);
    }
    Cell *c = &CELL(t, t->cy, t->cx);
    c->ch = cp;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attr = t->attr;
    if (t->cx < t->cols - 1) { t->cx++; t->wrap = false; }
    else t->wrap = true;
}

static void save_cursor(Term *t) {
    t->scx = t->cx; t->scy = t->cy;
    t->sfg = t->fg; t->sbg = t->bg; t->sattr = t->attr;
}

static void restore_cursor(Term *t) {
    t->cx = MIN(t->scx, t->cols - 1);
    t->cy = MIN(t->scy, t->rows - 1);
    t->fg = t->sfg; t->bg = t->sbg; t->attr = t->sattr;
    t->wrap = false;
}

static void set_alt(Term *t, bool on) {
    if (t->alt == on) return;
    t->alt = on;
    t->cells = on ? t->alt_buf : t->main_buf;
    if (on) {
        Cell d = { ' ', TERM_DEFAULT, TERM_DEFAULT, 0 };
        for (int i = 0; i < t->cols * t->rows; i++) t->cells[i] = d;
    }
    t->top = 0;
    t->bot = t->rows - 1;
}

static void reset_pen(Term *t) {
    t->fg = TERM_DEFAULT;
    t->bg = TERM_DEFAULT;
    t->attr = 0;
}

static void full_reset(Term *t) {
    set_alt(t, false);
    clear_images(t);
    reset_pen(t);
    fill(t, t->cells, t->cols * t->rows);
    t->cx = t->cy = 0;
    t->wrap = false;
    t->top = 0;
    t->bot = t->rows - 1;
    t->cursor_visible = true;
    t->app_cursor = false;
    t->autowrap = true;
    t->bracketed = false;
}

/* ---------- pty I/O ---------- */

void term_write(Term *t, const char *s, size_t n) {
    if (t->fd < 0) return;
    int spins = 0;
    while (n > 0) {
        ssize_t w = write(t->fd, s, n);
        if (w > 0) { s += w; n -= (size_t)w; spins = 0; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR) && spins++ < 200) { usleep(1000); continue; }
        break;
    }
}

static void reply(Term *t, const char *fmt, int a, int b) {
    char buf[48];
    int n = snprintf(buf, sizeof buf, fmt, a, b);
    if (n > 0) term_write(t, buf, (size_t)n);
}

/* ---------- parser ---------- */

static int P(const Term *t, int i, int def) {
    return (i < t->np && t->params[i] > 0) ? t->params[i] : def;
}

static void set_mode(Term *t, int m, bool on) {
    switch (m) {
    case 1:    t->app_cursor = on; break;
    case 7:    t->autowrap = on; break;
    case 25:   t->cursor_visible = on; break;
    case 47:
    case 1047: set_alt(t, on); break;
    case 1049:
        if (on) { save_cursor(t); set_alt(t, true); }
        else    { set_alt(t, false); restore_cursor(t); }
        break;
    case 2004: t->bracketed = on; break;
    default: break;
    }
}

static void sgr(Term *t) {
    int n = t->np ? t->np : 1;
    for (int i = 0; i < n; i++) {
        int p = i < t->np ? t->params[i] : 0;
        if (p == 0) reset_pen(t);
        else if (p == 1) t->attr |= ATTR_BOLD;
        else if (p == 4) t->attr |= ATTR_UL;
        else if (p == 7) t->attr |= ATTR_REV;
        else if (p == 22) t->attr &= (uint8_t)~ATTR_BOLD;
        else if (p == 24) t->attr &= (uint8_t)~ATTR_UL;
        else if (p == 27) t->attr &= (uint8_t)~ATTR_REV;
        else if (p >= 30 && p <= 37) t->fg = palette(t, p - 30);
        else if (p >= 40 && p <= 47) t->bg = palette(t, p - 40);
        else if (p >= 90 && p <= 97) t->fg = palette(t, p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = palette(t, p - 100 + 8);
        else if (p == 39) t->fg = TERM_DEFAULT;
        else if (p == 49) t->bg = TERM_DEFAULT;
        else if (p == 38 || p == 48) {
            uint32_t c = TERM_DEFAULT;
            bool ok = false;
            if (i + 2 < t->np && t->params[i + 1] == 5) {
                c = palette(t, t->params[i + 2]);
                i += 2; ok = true;
            } else if (i + 4 < t->np && t->params[i + 1] == 2) {
                c = (uint32_t)((t->params[i + 2] & 255) << 16 | (t->params[i + 3] & 255) << 8 | (t->params[i + 4] & 255));
                i += 4; ok = true;
            }
            if (ok) { if (p == 38) t->fg = c; else t->bg = c; }
        }
    }
}

static void csi_dispatch(Term *t, char f) {
    int n = P(t, 0, 1);
    switch (f) {
    case '@': { /* ICH */
        n = MIN(n, t->cols - t->cx);
        memmove(&CELL(t, t->cy, t->cx + n), &CELL(t, t->cy, t->cx), sizeof(Cell) * (size_t)(t->cols - t->cx - n));
        fill(t, &CELL(t, t->cy, t->cx), n);
        break;
    }
    case 'A': t->cy = (t->cy >= t->top) ? MAX(t->cy - n, t->top) : MAX(t->cy - n, 0); t->wrap = false; break;
    case 'B': t->cy = (t->cy <= t->bot) ? MIN(t->cy + n, t->bot) : MIN(t->cy + n, t->rows - 1); t->wrap = false; break;
    case 'C': t->cx = MIN(t->cx + n, t->cols - 1); t->wrap = false; break;
    case 'D': t->cx = MAX(t->cx - n, 0); t->wrap = false; break;
    case 'E': t->cy = MIN(t->cy + n, t->rows - 1); t->cx = 0; t->wrap = false; break;
    case 'F': t->cy = MAX(t->cy - n, 0); t->cx = 0; t->wrap = false; break;
    case 'G': t->cx = MIN(n, t->cols) - 1; t->wrap = false; break;
    case 'H':
    case 'f':
        t->cy = MIN(P(t, 0, 1), t->rows) - 1;
        t->cx = MIN(P(t, 1, 1), t->cols) - 1;
        t->wrap = false;
        break;
    case 'J': {
        int m = t->np ? t->params[0] : 0, cur = t->cy * t->cols + t->cx, all = t->rows * t->cols;
        erase_images(t, m, t->cy);
        if (m == 0) fill(t, &t->cells[cur], all - cur);
        else if (m == 1) fill(t, t->cells, cur + 1);
        else fill(t, t->cells, all);
        break;
    }
    case 'K': {
        int m = t->np ? t->params[0] : 0;
        if (m == 0) fill(t, &CELL(t, t->cy, t->cx), t->cols - t->cx);
        else if (m == 1) fill(t, &CELL(t, t->cy, 0), t->cx + 1);
        else fill(t, &CELL(t, t->cy, 0), t->cols);
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) scroll_down(t, t->cy, t->bot, n); break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) scroll_up(t, t->cy, t->bot, n); break;
    case 'P': { /* DCH */
        n = MIN(n, t->cols - t->cx);
        memmove(&CELL(t, t->cy, t->cx), &CELL(t, t->cy, t->cx + n), sizeof(Cell) * (size_t)(t->cols - t->cx - n));
        fill(t, &CELL(t, t->cy, t->cols - n), n);
        break;
    }
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'X': n = MIN(n, t->cols - t->cx); fill(t, &CELL(t, t->cy, t->cx), n); break;
    case 'd': t->cy = MIN(n, t->rows) - 1; t->wrap = false; break;
    case 'm': sgr(t); break;
    case 'r': {
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) { t->top = top; t->bot = bot; }
        t->cx = t->cy = 0;
        t->wrap = false;
        break;
    }
    case 's': if (!t->priv) save_cursor(t); break;
    case 'u': if (!t->priv) restore_cursor(t); break;
    case 'h':
    case 'l':
        if (t->priv == '?')
            for (int i = 0; i < (t->np ? t->np : 1); i++) set_mode(t, t->params[i], f == 'h');
        break;
    case 'n':
        if (t->priv) break;
        if (t->np && t->params[0] == 6) reply(t, "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
        else if (t->np && t->params[0] == 5) term_write(t, "\x1b[0n", 4);
        break;
    case 'c':
        if (t->priv == '>') term_write(t, "\x1b[>0;0;0c", 9);
        else if (!t->priv) term_write(t, "\x1b[?62;c", 7);
        break;
    default: break;
    }
}

static void put_str(Term *t, const char *s) {
    for (; *s; s++) put_char(t, (uint8_t)*s);
}

static void newline_cr(Term *t) {
    t->cx = 0;
    t->wrap = false;
    linefeed(t);
}

static void term_begin_focus_gallery(Term *t) {
    term_close_image_focus(t);
}

static void term_set_focus_image(Term *t, uint32_t *px, int w, int h) {
    if (t->nfocus == TERM_MAX_IMG) {
        free(t->focus_img[0].px);
        memmove(&t->focus_img[0], &t->focus_img[1],
                sizeof(t->focus_img[0]) * (TERM_MAX_IMG - 1));
        t->nfocus--;
        if (t->focus_index > 0) t->focus_index--;
    }
    if (!t->image_focus) {
        t->focus_index = 0;
        t->image_focus = true;
        t->focus_serial++;
    }
    t->focus_img[t->nfocus++] = (FocusImage){ .w = w, .h = h, .px = px };
}

void term_close_image_focus(Term *t) {
    for (int i = 0; i < t->nfocus; i++) free(t->focus_img[i].px);
    t->nfocus = 0;
    t->focus_index = 0;
    t->image_focus = false;
}

/* OSC 7770 ; mode ; /abs/path : display an image inline or in the focus viewer */
static void place_image(Term *t, const char *mode, const char *path) {
    if (t->alt || t->cell_w <= 0 || t->cell_h <= 0) return;
    size_t path_len = strlen(path);
    if (!path_len || path_len >= 4096) return;
    for (size_t i = 0; i < path_len; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 0x20 || ch == 0x7f) return;
    }

    if (!strcmp(mode, "focus")) {
        int w = 0, h = 0;
        uint32_t *px = image_load(path, 0, 0, t->bg_rgb, &w, &h);
        if (!px) {
            put_str(t, "image: cannot decode ");
            put_str(t, path);
            newline_cr(t);
            return;
        }
        term_set_focus_image(t, px, w, h);
        return;
    }
    if (strcmp(mode, "inline")) return;
    if (t->cx != 0) newline_cr(t);

    int maxh = (t->rows - 1) * t->cell_h;
    if (maxh < t->cell_h) maxh = t->cell_h;
    int w = 0, h = 0;
    uint32_t *px = image_load(path, t->cols * t->cell_w, maxh, t->bg_rgb, &w, &h);
    if (!px) {
        put_str(t, "image: cannot decode ");
        put_str(t, path);
        newline_cr(t);
        return;
    }
    term_close_image_focus(t);
    if (t->nimg == TERM_MAX_IMG) drop_image(t, 0);

    Placement *p = &t->img[t->nimg++];
    p->id = ++t->next_id;
    p->row = t->cy;
    p->col = 0;
    p->w = w;
    p->h = h;
    p->cols = (w + t->cell_w - 1) / t->cell_w;
    p->rows = (h + t->cell_h - 1) / t->cell_h;
    p->px = px;
    int rows = p->rows;
    for (int i = 0; i < rows; i++) linefeed(t); /* may scroll; placement scrolls with it */
    t->wrap = false;
}

static void place_video(Term *t, const char *path) {
    if (t->alt || t->cell_w <= 0 || t->cell_h <= 0) return;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof t->video_path) return;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 0x20 || ch == 0x7f) return;
    }

    if (t->cx != 0) newline_cr(t);
    t->video_row = t->cy;
    t->video_col = 0;
    memcpy(t->video_path, path, len + 1);
    t->video_serial++;
    t->video_cancelled = false;

    int max_w = t->cols * t->cell_w;
    int max_h = (t->rows > 1 ? t->rows - 1 : 1) * t->cell_h;
    if (max_w > VIDEO_MAX_WIDTH) max_w = VIDEO_MAX_WIDTH;
    if (max_h > VIDEO_MAX_HEIGHT) max_h = VIDEO_MAX_HEIGHT;
    t->video_max_w = max_w & ~1;
    t->video_max_h = max_h & ~1;

    t->video_rows = (t->video_max_h + t->cell_h - 1) / t->cell_h;
    for (int i = 0; i < t->video_rows; i++) linefeed(t);
    t->wrap = false;
}

static void osc_end(Term *t) {
    t->osc[t->osc_len] = 0;
    if (!strncmp(t->osc, "7770;", 5)) {
        char *payload = t->osc + 5;
        char *sep = strchr(payload, ';');
        size_t mode_len = sep ? (size_t)(sep - payload) : 0;
        if (sep && mode_len == 11 && !memcmp(payload, "focus-start", 11)) {
            term_begin_focus_gallery(t);
        } else if (sep && ((mode_len == 6 && !memcmp(payload, "inline", 6)) ||
                           (mode_len == 5 && !memcmp(payload, "focus", 5)))) {
            *sep = 0;
            place_image(t, payload, sep + 1);
        } else {
            place_image(t, "inline", payload);
        }
        return;
    }
    if (!strncmp(t->osc, "7771;", 5)) { place_video(t, t->osc + 5); return; }
    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') {
        strncpy(t->title, t->osc + 2, sizeof t->title - 1);
        t->title[sizeof t->title - 1] = 0;
    }
}

static void esc_byte(Term *t, uint8_t b) {
    t->state = S_GROUND;
    switch (b) {
    case '[':
        t->state = S_CSI;
        memset(t->params, 0, sizeof t->params);
        t->np = 0; t->priv = 0; t->inter = 0;
        break;
    case ']': t->state = S_OSC; t->osc_len = 0; break;
    case 'P': case 'X': case '^': case '_': t->state = S_STR; break;
    case '(': case ')': case '*': case '+': case '#': t->state = S_ESC_SKIP; break;
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'c': full_reset(t); break;
    default: break;
    }
}

static void csi_byte(Term *t, uint8_t b) {
    if (b >= '0' && b <= '9') {
        if (t->np == 0) t->np = 1;
        int *p = &t->params[t->np - 1];
        if (*p < 100000) *p = *p * 10 + (b - '0');
    } else if (b == ';' || b == ':') {
        if (t->np == 0) t->np = 1;
        if (t->np < TERM_MAXP) t->params[t->np++] = 0;
    } else if (b >= 0x3c && b <= 0x3f) {
        t->priv = (char)b;
    } else if (b >= 0x20 && b <= 0x2f) {
        t->inter = (char)b;
    } else if (b >= 0x40 && b <= 0x7e) {
        if (!t->inter) csi_dispatch(t, (char)b);
        t->state = S_GROUND;
    } else if (b == 0x1b) {
        t->state = S_ESC;
    }
}

static void ground_byte(Term *t, uint8_t b) {
    if (b == 0x1b) { t->state = S_ESC; t->utf_need = 0; return; }
    if (b < 0x20) {
        t->utf_need = 0;
        switch (b) {
        case '\n': case '\v': case '\f': linefeed(t); break;
        case '\r': t->cx = 0; t->wrap = false; break;
        case '\b': if (t->cx > 0) t->cx--; t->wrap = false; break;
        case '\t': t->cx = MIN((t->cx / 8 + 1) * 8, t->cols - 1); t->wrap = false; break;
        default: break;
        }
        return;
    }
    if (b == 0x7f) return;
    if (b < 0x80) { t->utf_need = 0; put_char(t, b); return; }
    if (t->utf_need > 0 && (b & 0xC0) == 0x80) {
        t->utf_cp = (t->utf_cp << 6) | (b & 0x3f);
        if (--t->utf_need == 0) put_char(t, t->utf_cp);
    } else if ((b & 0xE0) == 0xC0) { t->utf_cp = b & 0x1f; t->utf_need = 1; }
    else if ((b & 0xF0) == 0xE0)   { t->utf_cp = b & 0x0f; t->utf_need = 2; }
    else if ((b & 0xF8) == 0xF0)   { t->utf_cp = b & 0x07; t->utf_need = 3; }
    else { t->utf_need = 0; put_char(t, 0xFFFD); }
}

static void feed_byte(Term *t, uint8_t b) {
    if (b == 0x18 || b == 0x1a) { t->state = S_GROUND; return; }
    switch (t->state) {
    case S_GROUND:  ground_byte(t, b); break;
    case S_ESC:     esc_byte(t, b); break;
    case S_CSI:     csi_byte(t, b); break;
    case S_OSC:
        if (b == 7) { t->state = S_GROUND; osc_end(t); }
        else if (b == 0x1b) t->state = S_OSC_ESC;
        else if (t->osc_len < (int)sizeof t->osc - 1) t->osc[t->osc_len++] = (char)b;
        break;
    case S_OSC_ESC:
        t->state = S_GROUND;
        osc_end(t);
        if (b != '\\') esc_byte(t, b);
        break;
    case S_ESC_SKIP: t->state = S_GROUND; break;
    case S_STR:      if (b == 0x1b) t->state = S_STR_ESC; break;
    case S_STR_ESC:  t->state = (b == '\\') ? S_GROUND : S_STR; break;
    default:         t->state = S_GROUND; break;
    }
}

void term_feed(Term *t, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) feed_byte(t, (uint8_t)s[i]);
}

/* ---------- lifecycle ---------- */

void term_init(Term *t, int cols, int rows) {
    memset(t, 0, sizeof *t);
    memcpy(t->ansi, ansi16, sizeof ansi16);
    t->cols = cols;
    t->rows = rows;
    t->main_buf = alloc_buf(cols, rows);
    t->alt_buf = alloc_buf(cols, rows);
    t->cells = t->main_buf;
    t->fd = -1;
    t->pid = -1;
    t->alive = true;
    reset_pen(t);
    t->bot = rows - 1;
    t->cursor_visible = true;
    t->autowrap = true;
    strcpy(t->title, "shell");
}

static const char image_script[] =
    "#!/bin/sh\n"
    "mode=inline\n"
    "explicit=0\n"
    "case ${1-} in -inline) shift; explicit=1 ;; -focus) mode=focus; shift; explicit=1 ;; esac\n"
    "[ $# -ge 1 ] || { echo 'usage: .image [-inline|-focus] FILE...' >&2; exit 1; }\n"
    "[ \"$explicit\" -eq 1 ] || [ $# -le 5 ] || mode=focus\n"
    "[ \"$mode\" != focus ] || printf '\\033]7770;focus-start;\\007'\n"
    "for f in \"$@\"; do\n"
    "  case $f in /*) p=$f ;; *) p=$PWD/$f ;; esac\n"
    "  if [ -r \"$p\" ]; then printf '\\033]7770;%s;%s\\007' \"$mode\" \"$p\"\n"
    "  else echo \".image: cannot read $f\" >&2; fi\n"
    "done\n";

static const char video_script[] =
    "#!/bin/sh\n"
    "[ $# -eq 1 ] || { echo 'usage: .video FILE' >&2; exit 1; }\n"
    "f=$1\n"
    "case $f in /*) p=$f ;; *) p=$PWD/$f ;; esac\n"
    "if [ -r \"$p\" ]; then printf '\\033]7771;%s\\007' \"$p\"\n"
    "else echo \".video: cannot read $f\" >&2; exit 1; fi\n";

static void make_bin(Term *t) {
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    snprintf(t->bindir, sizeof t->bindir, "%s/clayterm-%d", base, (int)getpid());
    if (mkdir(t->bindir, 0700) && errno != EEXIST) { t->bindir[0] = 0; return; }
    char p[300];
    snprintf(p, sizeof p, "%s/.image", t->bindir);
    FILE *f = fopen(p, "w");
    if (!f) { t->bindir[0] = 0; return; }
    fputs(image_script, f);
    fclose(f);
    chmod(p, 0755);
    snprintf(p, sizeof p, "%s/.video", t->bindir);
    f = fopen(p, "w");
    if (!f) return;
    fputs(video_script, f);
    fclose(f);
    chmod(p, 0755);
}

bool term_spawn(Term *t) {
    struct winsize ws = { (unsigned short)t->rows, (unsigned short)t->cols, 0, 0 };
    make_bin(t);
    char newpath[4096] = "";
    if (t->bindir[0]) {
        const char *old = getenv("PATH");
        snprintf(newpath, sizeof newpath, "%s:%s", t->bindir, old ? old : "/usr/local/bin:/usr/bin:/bin");
    }
    pid_t pid = forkpty(&t->fd, NULL, NULL, &ws);
    if (pid < 0) { t->alive = false; return false; }
    if (pid == 0) {
        if (newpath[0]) setenv("PATH", newpath, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        execlp(sh, sh, (char *)NULL);
        _exit(127);
    }
    t->pid = pid;
    fcntl(t->fd, F_SETFL, fcntl(t->fd, F_GETFL) | O_NONBLOCK);
    return true;
}

void term_free(Term *t) {
    clear_images(t);
    term_close_image_focus(t);
    if (t->bindir[0]) {
        char p[300];
        snprintf(p, sizeof p, "%s/.image", t->bindir);
        unlink(p);
        snprintf(p, sizeof p, "%s/.video", t->bindir);
        unlink(p);
        rmdir(t->bindir);
    }
    if (t->fd >= 0) close(t->fd);
    if (t->pid > 0) { kill(t->pid, SIGHUP); waitpid(t->pid, NULL, WNOHANG); }
    free(t->main_buf);
    free(t->alt_buf);
    t->main_buf = t->alt_buf = t->cells = NULL;
}

bool term_pump(Term *t) {
    char buf[16384];
    bool got = false;
    for (int i = 0; i < 16 && t->fd >= 0; i++) {
        ssize_t n = read(t->fd, buf, sizeof buf);
        if (n > 0) { term_feed(t, buf, (size_t)n); got = true; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        t->alive = false; /* EOF / EIO */
        break;
    }
    if (t->pid > 0 && waitpid(t->pid, NULL, WNOHANG) > 0) { t->pid = -1; t->alive = false; }
    return got;
}

void term_resize(Term *t, int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;

    Cell *nm = alloc_buf(cols, rows), *na = alloc_buf(cols, rows);
    int off = (t->cy >= rows) ? t->cy - rows + 1 : 0;
    if (off) shift_images(t, -off);
    int offm = t->alt ? 0 : off, offa = t->alt ? off : 0;
    int cc = MIN(cols, t->cols);

    for (int r = 0; r < rows; r++) {
        if (r + offm < t->rows) memcpy(&nm[r * cols], &t->main_buf[(r + offm) * t->cols], sizeof(Cell) * (size_t)cc);
        if (r + offa < t->rows) memcpy(&na[r * cols], &t->alt_buf[(r + offa) * t->cols], sizeof(Cell) * (size_t)cc);
    }
    free(t->main_buf);
    free(t->alt_buf);
    t->main_buf = nm;
    t->alt_buf = na;
    t->cells = t->alt ? na : nm;
    t->cy = MIN(t->cy - off, rows - 1);
    t->cx = MIN(t->cx, cols - 1);
    t->scx = MIN(t->scx, cols - 1);
    t->scy = MIN(t->scy, rows - 1);
    t->cols = cols;
    t->rows = rows;
    t->top = 0;
    t->bot = rows - 1;
    t->wrap = false;

    if (t->fd >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(t->fd, TIOCSWINSZ, &ws);
    }
}
