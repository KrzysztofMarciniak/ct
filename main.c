#define CLAY_IMPLEMENTATION
#include "clay.h" /* before X11: Xlib's macros must not leak into Clay */

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "term.h"
#include "video.h"

#define PAD 0
#define FOCUS_BAR_H 42

#define DEFAULT_BG     0x1a1b26u
#define DEFAULT_FG     0xc0caf5u
#define DEFAULT_CURSOR 0xc0caf5u

static Display *dpy;
static int      scr;
static Visual  *vis;
static Colormap cmap;
static Window   win;
static Pixmap   back;
static XftDraw *draw;
static GC       gc;
static XftFont *tfont;
static XIC      ic;
static Atom     a_delete, a_clip, a_utf8, a_prop;
static int      winw = 960, winh = 600;
static int      g_cw, g_ch;
static bool     focused = true, quit, dirty = true;
static Window   video_window;
static bool     video_window_mapped;
static double   image_zoom = 1.0;
static int      image_pan_x, image_pan_y;
static uint64_t image_focus_serial;
static XrmDatabase xrdb;
static uint32_t bg_rgb = DEFAULT_BG, fg_rgb = DEFAULT_FG, cursor_rgb = DEFAULT_CURSOR;
static char font_pattern[256] = "monospace:pixelsize=18";

static Term term;
static Video video;
static uint64_t video_serial;

/* ---------- helpers ---------- */

static Clay_Color clay_rgb(uint32_t rgb) {
    return (Clay_Color){ (float)((rgb >> 16) & 255), (float)((rgb >> 8) & 255), (float)(rgb & 255), 255 };
}

static XftColor xc(unsigned r, unsigned g, unsigned b) {
    XRenderColor rc = { (unsigned short)(r * 257), (unsigned short)(g * 257), (unsigned short)(b * 257), 0xffff };
    XftColor c;
    XftColorAllocValue(dpy, vis, cmap, &rc, &c);
    return c;
}

static XftColor xc_rgb(uint32_t c) { return xc((c >> 16) & 255, (c >> 8) & 255, c & 255); }

static bool resource_value(const char *name, const char *class_name, XrmValue *value) {
    char *type;
    return xrdb && XrmGetResource(xrdb, (char *)name, (char *)class_name, &type, value);
}

static uint32_t resource_color(const char *name, const char *class_name, uint32_t fallback) {
    XrmValue value;
    if (!resource_value(name, class_name, &value)) return fallback;
    XColor color;
    if (!XParseColor(dpy, cmap, value.addr, &color)) {
        fprintf(stderr, "clayterm: invalid Xresource color %s: %s\n", name, value.addr);
        return fallback;
    }
    return (uint32_t)(color.red >> 8) << 16 |
           (uint32_t)(color.green >> 8) << 8 |
           (uint32_t)(color.blue >> 8);
}

static void load_resources(void) {
    XrmInitialize();
    const char *database = XResourceManagerString(dpy);
    if (database) xrdb = XrmGetStringDatabase(database);

    bg_rgb = resource_color("clayterm.background", "Clayterm.Background", DEFAULT_BG);
    fg_rgb = resource_color("clayterm.foreground", "Clayterm.Foreground", DEFAULT_FG);
    cursor_rgb = resource_color("clayterm.cursorColor", "Clayterm.CursorColor", fg_rgb);

    const char *env_font = getenv("CLAYTERM_FONT");
    if (env_font && *env_font) snprintf(font_pattern, sizeof font_pattern, "%s", env_font);
    XrmValue value;
    if (resource_value("clayterm.font", "Clayterm.Font", &value))
        snprintf(font_pattern, sizeof font_pattern, "%s", value.addr);
}

static unsigned long pixel_rgb(uint32_t rgb) {
    XColor color = {
        .red = (unsigned short)(((rgb >> 16) & 255) * 257),
        .green = (unsigned short)(((rgb >> 8) & 255) * 257),
        .blue = (unsigned short)((rgb & 255) * 257),
        .flags = DoRed | DoGreen | DoBlue,
    };
    if (!XAllocColor(dpy, cmap, &color)) return BlackPixel(dpy, scr);
    return color.pixel;
}

static void on_clay_error(Clay_ErrorData e) {
    fprintf(stderr, "clay: %.*s\n", (int)e.errorText.length, e.errorText.chars);
}

static bool video_geometry(const Term *t, int *x, int *y, unsigned *w, unsigned *h) {
    int left = PAD, top = PAD;
    int right = winw - PAD, bottom = winh - PAD;
    *x = left + t->video_col * g_cw;
    *y = top + t->video_row * g_ch;
    if (*x < left) *x = left;
    if (*y < top) *y = top;
    if (*x >= right || *y >= bottom) return false;

    int width = t->video_max_w;
    int height = t->video_max_h;
    if (width > right - *x) width = right - *x;
    if (height > bottom - *y) height = bottom - *y;
    if (width < 2 || height < 2) return false;
    *w = (unsigned)width;
    *h = (unsigned)height;
    return true;
}

static void position_video_window(const Term *t) {
    if (!video_window) return;
    int x, y;
    unsigned w, h;
    if (!video_geometry(t, &x, &y, &w, &h)) {
        if (video_window_mapped) XUnmapWindow(dpy, video_window);
        video_window_mapped = false;
        return;
    }
    XMoveResizeWindow(dpy, video_window, x, y, w, h);
    if (!video_window_mapped) {
        XMapRaised(dpy, video_window);
        video_window_mapped = true;
    }
}

static void stop_embedded_video(void) {
    video_stop(&video);
    if (video_window) XDestroyWindow(dpy, video_window);
    video_window = 0;
    video_window_mapped = false;
}

static bool start_embedded_video(const Term *t) {
    int x, y;
    unsigned w, h;
    if (!video_geometry(t, &x, &y, &w, &h)) {
        fprintf(stderr, "clayterm: no room to display video\n");
        return false;
    }
    video_window = XCreateSimpleWindow(dpy, win, x, y, w, h, 0,
                                       BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XMapRaised(dpy, video_window);
    video_window_mapped = true;
    XSync(dpy, False);
    if (!video_start(&video, t->video_path, (int64_t)video_window)) {
        XDestroyWindow(dpy, video_window);
        video_window = 0;
        video_window_mapped = false;
        return false;
    }
    return true;
}

/* ---------- image pixmap cache (server side, keyed by placement id) ---------- */

static struct { int id; Pixmap pm; } icache[TERM_MAX_IMG];

static Pixmap img_pixmap(const Placement *p) {
    int free_slot = -1;
    for (int i = 0; i < TERM_MAX_IMG; i++) {
        if (icache[i].id == p->id) return icache[i].pm;
        if (!icache[i].id && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;
    int depth = DefaultDepth(dpy, scr);
    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)p->w, (unsigned)p->h, (unsigned)depth);
    XImage *xi = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, (char *)p->px, (unsigned)p->w, (unsigned)p->h, 32, 0);
    if (!xi) { XFreePixmap(dpy, pm); return 0; }
    XPutImage(dpy, pm, gc, xi, 0, 0, 0, 0, (unsigned)p->w, (unsigned)p->h);
    xi->data = NULL; /* data belongs to the terminal */
    XDestroyImage(xi);
    icache[free_slot].id = p->id;
    icache[free_slot].pm = pm;
    return pm;
}

static void prune_icache(const Term *t) {
    for (int i = 0; i < TERM_MAX_IMG; i++) {
        if (!icache[i].id) continue;
        bool live = false;
        for (int j = 0; j < t->nimg; j++) if (t->img[j].id == icache[i].id) { live = true; break; }
        if (!live) { XFreePixmap(dpy, icache[i].pm); icache[i].id = 0; }
    }
}

/* ---------- terminal rendering ---------- */

static void draw_term(const Term *t, Clay_BoundingBox b) {
    int x0 = (int)b.x, y0 = (int)b.y;
    int base = (g_ch - tfont->height) / 2 + tfont->ascent;

    if (!t->alt) {
        for (int i = 0; i < t->nimg; i++) {
            const Placement *p = &t->img[i];
            Pixmap pm = img_pixmap(p);
            if (!pm) continue;
            int sy = p->row < 0 ? -p->row * g_ch : 0;
            int dy = y0 + (p->row < 0 ? 0 : p->row * g_ch);
            if (sy >= p->h) continue;
            XCopyArea(dpy, pm, back, gc, 0, sy, (unsigned)p->w, (unsigned)(p->h - sy), x0 + p->col * g_cw, dy);
        }
    }

    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            const Cell *e = &t->cells[r * t->cols + c];
            bool rev = (e->attr & ATTR_REV) != 0;
            uint32_t f = (e->fg & TERM_DEFAULT) ? fg_rgb : e->fg;
            uint32_t g = (e->bg & TERM_DEFAULT) ? bg_rgb : e->bg;
            bool hasbg = !(e->bg & TERM_DEFAULT) || rev;
            if (rev) { uint32_t tmp = f; f = g; g = tmp; }
            int x = x0 + c * g_cw, y = y0 + r * g_ch;
            if (hasbg) { XftColor bc = xc_rgb(g); XftDrawRect(draw, &bc, x, y, (unsigned)g_cw, (unsigned)g_ch); }
            if (e->ch > ' ' || (e->attr & ATTR_UL)) {
                XftColor fc = xc_rgb(f);
                if (e->ch > ' ') {
                    FcChar32 ch = e->ch;
                    XftDrawString32(draw, &fc, tfont, x, y + base, &ch, 1);
                    if (e->attr & ATTR_BOLD) XftDrawString32(draw, &fc, tfont, x + 1, y + base, &ch, 1);
                }
                if (e->attr & ATTR_UL) XftDrawRect(draw, &fc, x, y + g_ch - 2, (unsigned)g_cw, 1);
            }
        }
    }

    if (t->cursor_visible && !t->image_focus) {
        int x = x0 + t->cx * g_cw, y = y0 + t->cy * g_ch;
        XftColor cc = xc_rgb(cursor_rgb);
        if (focused) {
            XftDrawRect(draw, &cc, x, y, (unsigned)g_cw, (unsigned)g_ch);
            const Cell *e = &t->cells[t->cy * t->cols + t->cx];
            if (e->ch > ' ') {
                XftColor bc = xc_rgb(bg_rgb);
                FcChar32 ch = e->ch;
                XftDrawString32(draw, &bc, tfont, x, y + base, &ch, 1);
            }
        } else {
            XSetForeground(dpy, gc, cc.pixel);
            XDrawRectangle(dpy, back, gc, x, y, (unsigned)g_cw - 1, (unsigned)g_ch - 1);
        }
    }
}

static void render(Clay_RenderCommandArray cmds) {
    for (int i = 0; i < cmds.length; i++) {
        Clay_RenderCommand *c = &cmds.internalArray[i];
        Clay_BoundingBox b = c->boundingBox;
        switch (c->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *r = &c->renderData.rectangle;
            XftColor col = xc((unsigned)r->backgroundColor.r, (unsigned)r->backgroundColor.g, (unsigned)r->backgroundColor.b);
            if (r->cornerRadius.topLeft > 0) {
                XSetForeground(dpy, gc, col.pixel);
                XFillArc(dpy, back, gc, (int)b.x, (int)b.y, (unsigned)b.width, (unsigned)b.height, 0, 360 * 64);
            } else {
                XftDrawRect(draw, &col, (int)b.x, (int)b.y, (unsigned)b.width, (unsigned)b.height);
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            if (c->renderData.custom.customData) draw_term(c->renderData.custom.customData, b);
            break;
        default:
            break;
        }
    }
}

static double focused_image_scale(const Term *t, int width, int height) {
    const FocusImage *image = &t->focus_img[t->focus_index];
    double sx = (double)width / image->w;
    double sy = (double)height / image->h;
    double scale = sx < sy ? sx : sy;
    return scale * image_zoom;
}

static void clamp_image_pan(const Term *t) {
    if (!t->nfocus) return;
    int height = winh - FOCUS_BAR_H;
    if (height < 1) height = 1;
    double scale = focused_image_scale(t, winw, height);
    const FocusImage *image = &t->focus_img[t->focus_index];
    double max_x = ((double)image->w * scale - winw) / 2.0;
    double max_y = ((double)image->h * scale - height) / 2.0;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (image_pan_x > max_x) image_pan_x = (int)max_x;
    if (image_pan_x < -max_x) image_pan_x = (int)-max_x;
    if (image_pan_y > max_y) image_pan_y = (int)max_y;
    if (image_pan_y < -max_y) image_pan_y = (int)-max_y;
}

static void zoom_image(double factor) {
    image_zoom *= factor;
    if (image_zoom < 0.25) image_zoom = 0.25;
    if (image_zoom > 8.0) image_zoom = 8.0;
    clamp_image_pan(&term);
    dirty = true;
}

static void draw_focused_image(const Term *t) {
    if (!t->image_focus || !t->nfocus) return;
    const FocusImage *image = &t->focus_img[t->focus_index];
    if (!image->px || image->w <= 0 || image->h <= 0) return;

    int view_h = winh - FOCUS_BAR_H;
    if (view_h < 1) view_h = 1;
    XftColor bg = xc_rgb(bg_rgb);
    XftDrawRect(draw, &bg, 0, 0, (unsigned)winw, (unsigned)winh);

    double scale = focused_image_scale(t, winw, view_h);
    double image_w = (double)image->w * scale;
    double image_h = (double)image->h * scale;
    int image_x = (int)((winw - image_w) / 2.0 + image_pan_x);
    int image_y = (int)((view_h - image_h) / 2.0 + image_pan_y);
    int left = image_x < 0 ? 0 : image_x;
    int top = image_y < 0 ? 0 : image_y;
    int right = image_x + (int)image_w;
    int bottom = image_y + (int)image_h;
    if (right > winw) right = winw;
    if (bottom > view_h) bottom = view_h;

    if (right > left && bottom > top) {
        int width = right - left, height = bottom - top;
        XImage *xi = XCreateImage(dpy, vis, (unsigned)DefaultDepth(dpy, scr), ZPixmap,
                                  0, NULL, (unsigned)width, (unsigned)height, 32, 0);
        if (xi) {
            size_t bytes = (size_t)xi->bytes_per_line * (size_t)height;
            xi->data = calloc(1, bytes);
            if (xi->data) {
                for (int y = 0; y < height; y++) {
                    int sy = (int)(((top + y - image_y) / scale));
                    if (sy < 0) sy = 0;
                    if (sy >= image->h) sy = image->h - 1;
                    for (int x = 0; x < width; x++) {
                        int sx = (int)(((left + x - image_x) / scale));
                        if (sx < 0) sx = 0;
                        if (sx >= image->w) sx = image->w - 1;
                        unsigned long pixel = image->px[(size_t)sy * (size_t)image->w + (size_t)sx];
                        XPutPixel(xi, x, y, pixel);
                    }
                }
                XPutImage(dpy, back, gc, xi, 0, 0, left, top, (unsigned)width, (unsigned)height);
            }
            XDestroyImage(xi);
        }
    }

    XftColor bar = xc(0x24, 0x26, 0x33);
    XftDrawRect(draw, &bar, 0, winh - FOCUS_BAR_H, (unsigned)winw, FOCUS_BAR_H);
    XftColor fg = xc_rgb(fg_rgb);
    char controls[160];
    snprintf(controls, sizeof controls,
             "Image %d/%d   Zoom %d%%   h/j/k/l pan   Shift+h/l prev/next   +/- zoom   0 reset   Esc/q close",
             t->focus_index + 1, t->nfocus, (int)(image_zoom * 100.0 + 0.5));
    XftDrawStringUtf8(draw, &fg, tfont, 16, winh - 15,
                      (const FcChar8 *)controls, (int)strlen(controls));
}

static void make_backbuffer(void) {
    if (back) XFreePixmap(dpy, back);
    back = XCreatePixmap(dpy, win, (unsigned)winw, (unsigned)winh, (unsigned)DefaultDepth(dpy, scr));
    if (draw) XftDrawChange(draw, back);
    else draw = XftDrawCreate(dpy, back, vis, cmap);
}

static void redraw(void) {
    prune_icache(&term);
    if (term.image_focus && image_focus_serial != term.focus_serial) {
        image_focus_serial = term.focus_serial;
        image_zoom = 1.0;
        image_pan_x = image_pan_y = 0;
    }
    if (term.image_focus) clamp_image_pan(&term);
    Clay_SetLayoutDimensions((Clay_Dimensions){ (float)winw, (float)winh });

    Clay_BeginLayout();
    CLAY({ .id = CLAY_ID("Root"),
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .backgroundColor = clay_rgb(bg_rgb),
    }) {
        CLAY({ .id = CLAY_ID("Term"),
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
            .custom = { .customData = &term },
        }) {}
    }
    Clay_RenderCommandArray cmds = Clay_EndLayout();

    Clay_ElementData ed = Clay_GetElementData(CLAY_ID("Term"));
    if (ed.found) {
        int cols = (int)(ed.boundingBox.width / (float)g_cw);
        int rows = (int)(ed.boundingBox.height / (float)g_ch);
        if (cols > 0 && rows > 0) term_resize(&term, cols, rows);
    }
    position_video_window(&term);

    XftColor bg = xc_rgb(bg_rgb);
    XftDrawRect(draw, &bg, 0, 0, (unsigned)winw, (unsigned)winh);
    render(cmds);
    draw_focused_image(&term);
    XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)winw, (unsigned)winh, 0, 0);
    XFlush(dpy);
}

/* ---------- input ---------- */

static const struct { KeySym key; const char *norm, *app; } special[] = {
    { XK_Up, "\x1b[A", "\x1bOA" },        { XK_Down, "\x1b[B", "\x1bOB" },
    { XK_Right, "\x1b[C", "\x1bOC" },     { XK_Left, "\x1b[D", "\x1bOD" },
    { XK_Home, "\x1b[H", "\x1bOH" },      { XK_End, "\x1b[F", "\x1bOF" },
    { XK_Insert, "\x1b[2~", "\x1b[2~" },  { XK_Delete, "\x1b[3~", "\x1b[3~" },
    { XK_Page_Up, "\x1b[5~", "\x1b[5~" }, { XK_Page_Down, "\x1b[6~", "\x1b[6~" },
    { XK_Return, "\r", "\r" },            { XK_KP_Enter, "\r", "\r" },
    { XK_BackSpace, "\x7f", "\x7f" },     { XK_Tab, "\t", "\t" },
    { XK_Escape, "\x1b", "\x1b" },
    { XK_F1, "\x1bOP", "\x1bOP" },        { XK_F2, "\x1bOQ", "\x1bOQ" },
    { XK_F3, "\x1bOR", "\x1bOR" },        { XK_F4, "\x1bOS", "\x1bOS" },
    { XK_F5, "\x1b[15~", "\x1b[15~" },    { XK_F6, "\x1b[17~", "\x1b[17~" },
    { XK_F7, "\x1b[18~", "\x1b[18~" },    { XK_F8, "\x1b[19~", "\x1b[19~" },
    { XK_F9, "\x1b[20~", "\x1b[20~" },    { XK_F10, "\x1b[21~", "\x1b[21~" },
    { XK_F11, "\x1b[23~", "\x1b[23~" },   { XK_F12, "\x1b[24~", "\x1b[24~" },
};

static void request_paste(Atom sel) {
    XConvertSelection(dpy, sel, a_utf8, a_prop, win, CurrentTime);
}

static void paste(const char *s, size_t n) {
    if (term.bracketed) term_write(&term, "\x1b[200~", 6);
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (!term.bracketed && ch == '\n') ch = '\r';
        term_write(&term, &ch, 1);
    }
    if (term.bracketed) term_write(&term, "\x1b[201~", 6);
}

static void on_selection(XSelectionEvent *se) {
    if (se->property == None) return;
    Atom type;
    int fmt;
    unsigned long n, rest;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, a_prop, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &rest, &data) == Success
        && data && fmt == 8)
        paste((const char *)data, n);
    if (data) XFree(data);
}

static void on_key(XKeyEvent *ke) {
    char buf[64];
    KeySym ks = NoSymbol;
    int n;
    Status st = 0;
    if (ic) {
        n = Xutf8LookupString(ic, ke, buf, sizeof buf - 1, &ks, &st);
        if (st == XBufferOverflow || st == XLookupNone) return;
    } else {
        n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    }
    bool ctrl = ke->state & ControlMask, alt = ke->state & Mod1Mask, shift = ke->state & ShiftMask;

    if (term.image_focus) {
        if (ks == XK_Escape || ks == XK_q || ks == XK_Q) {
            term_close_image_focus(&term);
            dirty = true;
        } else if (term.nfocus > 1 && (ks == XK_H || ks == XK_L)) {
            int previous = term.focus_index;
            if (ks == XK_H && term.focus_index > 0) term.focus_index--;
            if (ks == XK_L && term.focus_index < term.nfocus - 1) term.focus_index++;
            if (term.focus_index != previous) {
                term.focus_serial++;
                dirty = true;
            }
        } else if (ks == XK_plus || ks == XK_equal || ks == XK_KP_Add) {
            zoom_image(1.25);
        } else if (ks == XK_minus || ks == XK_KP_Subtract) {
            zoom_image(1.0 / 1.25);
        } else if (ks == XK_0 || ks == XK_Home) {
            image_zoom = 1.0;
            image_pan_x = image_pan_y = 0;
            dirty = true;
        } else if (ks == XK_Left || ks == XK_Right || ks == XK_Up || ks == XK_Down ||
                   ks == XK_h || ks == XK_l || ks == XK_k || ks == XK_j) {
            if (ks == XK_Left || ks == XK_h) image_pan_x += 60;
            if (ks == XK_Right || ks == XK_l) image_pan_x -= 60;
            if (ks == XK_Up || ks == XK_k) image_pan_y += 60;
            if (ks == XK_Down || ks == XK_j) image_pan_y -= 60;
            clamp_image_pan(&term);
            dirty = true;
        }
        return;
    }

    if (ctrl && shift && video.active && ks == XK_space) { video_toggle_pause(&video); return; }
    if (ctrl && shift && video.active && (ks == XK_Left || ks == XK_Right)) {
        video_seek(&video, ks == XK_Left ? -5.0 : 5.0);
        return;
    }
    if (ctrl && shift && (ks == XK_V || ks == XK_v)) { request_paste(a_clip); return; }
    if (shift && ks == XK_Insert) { request_paste(XA_PRIMARY); return; }
    if (ks == XK_ISO_Left_Tab) { term_write(&term, "\x1b[Z", 3); return; }

    for (size_t i = 0; i < sizeof special / sizeof *special; i++) {
        if (special[i].key != ks) continue;
        const char *s = term.app_cursor ? special[i].app : special[i].norm;
        if (alt && strlen(s) == 1) term_write(&term, "\x1b", 1);
        term_write(&term, s, strlen(s));
        return;
    }
    if (n > 0) {
        if (alt) term_write(&term, "\x1b", 1);
        term_write(&term, buf, (size_t)n);
    }
}

/* ---------- main ---------- */

int main(void) {
    setlocale(LC_ALL, "");
    XSetLocaleModifiers("");
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "clayterm: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    vis = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);
    load_resources();

    tfont = XftFontOpenName(dpy, scr, font_pattern);
    if (!tfont && strcmp(font_pattern, "monospace:pixelsize=18")) {
        fprintf(stderr, "clayterm: cannot open Xresources font %s; using default\n", font_pattern);
        tfont = XftFontOpenName(dpy, scr, "monospace:pixelsize=18");
    }
    if (!tfont) { fprintf(stderr, "clayterm: cannot open font\n"); return 1; }
    XGlyphInfo gi;
    FcChar32 m = 'M';
    XftTextExtents32(dpy, tfont, &m, 1, &gi);
    g_cw = gi.xOff > 0 ? gi.xOff : tfont->max_advance_width;
    g_ch = tfont->height + 2;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)winw, (unsigned)winh,
                              0, pixel_rgb(bg_rgb), pixel_rgb(bg_rgb));
    XSetWindowBackgroundPixmap(dpy, win, None);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask | FocusChangeMask |
                           ButtonPressMask);
    a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_prop = XInternAtom(dpy, "CLAYTERM_SEL", False);
    XSetWMProtocols(dpy, win, &a_delete, 1);
    XClassHint class_hint = { .res_name = "clayterm", .res_class = "Clayterm" };
    XSetClassHint(dpy, win, &class_hint);
    Xutf8SetWMProperties(dpy, win, "clayterm", "clayterm", NULL, 0, NULL, NULL, NULL);
    gc = XCreateGC(dpy, win, 0, NULL);
    make_backbuffer();

    XIM xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim)
        ic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win,
                       XNFocusWindow, win, NULL);
    XMapWindow(dpy, win);

    uint32_t mem = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem));
    Clay_Initialize(arena, (Clay_Dimensions){ (float)winw, (float)winh }, (Clay_ErrorHandler){ on_clay_error, 0 });

    term_init(&term, 80, 24);
    video_init(&video);
    term.cell_w = g_cw;
    term.cell_h = g_ch;
    term.bg_rgb = bg_rgb;
    for (int i = 0; i < 16; i++) {
        char name[64], class_name[64];
        snprintf(name, sizeof name, "clayterm.color%d", i);
        snprintf(class_name, sizeof class_name, "Clayterm.Color%d", i);
        term.ansi[i] = resource_color(name, class_name, term.ansi[i]);
    }
    if (!term_spawn(&term)) { fprintf(stderr, "clayterm: forkpty failed\n"); return 1; }

    char last_title[128] = "";
    int xfd = ConnectionNumber(dpy);

    while (!quit) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (XFilterEvent(&ev, None)) continue;
            switch (ev.type) {
            case Expose:        dirty = true; break;
            case KeyPress:      on_key(&ev.xkey); break;
            case FocusIn:       focused = true; dirty = true; break;
            case FocusOut:      focused = false; dirty = true; break;
            case ButtonPress:
                if (term.image_focus) {
                    if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                        zoom_image(ev.xbutton.button == Button4 ? 1.25 : 1.0 / 1.25);
                } else if (ev.xbutton.button == Button2) {
                    request_paste(XA_PRIMARY);
                }
                break;
            case SelectionNotify: on_selection(&ev.xselection); break;
            case ConfigureNotify:
                if (ev.xconfigure.width != winw || ev.xconfigure.height != winh) {
                    winw = ev.xconfigure.width;
                    winh = ev.xconfigure.height;
                    make_backbuffer();
                    dirty = true;
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == a_delete) quit = true;
                break;
            default: break;
            }
        }
        if (quit) break;

        if (term_pump(&term)) dirty = true;
        if (!term.alive) break;

        if (term.video_cancelled) {
            stop_embedded_video();
            video_serial = term.video_serial;
            term.video_cancelled = false;
            dirty = true;
        } else if (term.video_serial != video_serial) {
            stop_embedded_video();
            video_serial = term.video_serial;
            start_embedded_video(&term);
            dirty = true;
        }

        if (strcmp(last_title, term.title)) {
            snprintf(last_title, sizeof last_title, "%s", term.title);
            Xutf8SetWMProperties(dpy, win, term.title, term.title, NULL, 0, NULL, NULL, NULL);
        }
        if (dirty) { redraw(); dirty = false; }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(xfd, &rf);
        FD_SET(term.fd, &rf);
        int nfd = (xfd > term.fd ? xfd : term.fd) + 1;
        select(nfd, &rf, NULL, NULL, NULL);
    }

    stop_embedded_video();
    term_free(&term);
    return 0;
}
