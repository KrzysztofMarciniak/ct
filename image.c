#include "image.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

/* ---------- decoders: all return malloc'd RGBA8 ---------- */

static uint8_t *decode_png(const char *path, int *w, int *h) {
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path)) return NULL;
    im.format = PNG_FORMAT_RGBA;
    uint8_t *buf = malloc(PNG_IMAGE_SIZE(im));
    if (!buf || !png_image_finish_read(&im, NULL, buf, 0, NULL)) {
        free(buf);
        png_image_free(&im);
        return NULL;
    }
    *w = (int)im.width;
    *h = (int)im.height;
    return buf;
}

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jerr_exit(j_common_ptr c) {
    longjmp(((struct jerr *)c->err)->jb, 1);
}

static uint8_t *decode_jpeg(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct jpeg_decompress_struct ci;
    struct jerr je;
    uint8_t *volatile out = NULL;
    uint8_t *volatile row = NULL;

    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        fclose(f);
        free(out);
        free(row);
        return NULL;
    }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_read_header(&ci, TRUE);
    ci.out_color_space = JCS_RGB;
    jpeg_start_decompress(&ci);

    size_t W = ci.output_width, H = ci.output_height;
    out = malloc(W * H * 4);
    row = malloc(W * 3);
    if (!out || !row) longjmp(je.jb, 1);

    while (ci.output_scanline < ci.output_height) {
        size_t y = ci.output_scanline;
        JSAMPROW rp = row;
        jpeg_read_scanlines(&ci, &rp, 1);
        uint8_t *d = out + y * W * 4;
        for (size_t x = 0; x < W; x++) {
            d[x * 4 + 0] = row[x * 3 + 0];
            d[x * 4 + 1] = row[x * 3 + 1];
            d[x * 4 + 2] = row[x * 3 + 2];
            d[x * 4 + 3] = 255;
        }
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    fclose(f);
    free(row);
    *w = (int)W;
    *h = (int)H;
    return out;
}

static uint8_t *decode_webp(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (256L << 20)) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f);

    int iw = 0, ih = 0;
    uint8_t *rgba = WebPDecodeRGBA(data, (size_t)n, &iw, &ih);
    free(data);
    if (!rgba) return NULL;
    size_t sz = (size_t)iw * (size_t)ih * 4;
    uint8_t *out = malloc(sz);
    if (out) memcpy(out, rgba, sz);
    WebPFree(rgba);
    *w = iw;
    *h = ih;
    return out;
}

/* ---------- load + scale ---------- */

uint32_t *image_load(const char *path, int max_w, int max_h, uint32_t bg, int *ow, int *oh) {
    uint8_t magic[12] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t m = fread(magic, 1, sizeof magic, f);
    fclose(f);

    int w = 0, h = 0;
    uint8_t *src = NULL;
    if (m >= 8 && !memcmp(magic, "\x89PNG\r\n\x1a\n", 8)) src = decode_png(path, &w, &h);
    else if (m >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) src = decode_jpeg(path, &w, &h);
    else if (m >= 12 && !memcmp(magic, "RIFF", 4) && !memcmp(magic + 8, "WEBP", 4)) src = decode_webp(path, &w, &h);
    if (!src || w <= 0 || h <= 0) { free(src); return NULL; }

    double s = 1.0;
    if (max_w > 0 && (double)w * s > max_w) s = (double)max_w / w;
    if (max_h > 0 && (double)h * s > max_h) s = (double)max_h / h;
    int dw = (int)(w * s + 0.5), dh = (int)(h * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint32_t *dst = malloc((size_t)dw * (size_t)dh * sizeof *dst);
    if (!dst) { free(src); return NULL; }

    uint32_t br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = (int)((long long)dy * h / dh), sy1 = (int)((long long)(dy + 1) * h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = (int)((long long)dx * w / dw), sx1 = (int)((long long)(dx + 1) * w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0;
            uint32_t cnt = 0;
            for (int y = sy0; y < sy1 && y < h; y++) {
                const uint8_t *p = src + ((size_t)y * (size_t)w + (size_t)sx0) * 4;
                for (int x = sx0; x < sx1 && x < w; x++, p += 4) {
                    uint32_t a = p[3];
                    r += (p[0] * a + br * (255 - a)) / 255;
                    g += (p[1] * a + bgc * (255 - a)) / 255;
                    b += (p[2] * a + bb * (255 - a)) / 255;
                    cnt++;
                }
            }
            if (!cnt) cnt = 1;
            dst[(size_t)dy * (size_t)dw + (size_t)dx] =
                (uint32_t)((r / cnt) << 16 | (g / cnt) << 8 | (b / cnt));
        }
    }
    free(src);
    *ow = dw;
    *oh = dh;
    return dst;
}
