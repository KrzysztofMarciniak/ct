#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

/* Decode png/jpg/webp. Composite over bg (0xRRGGBB), box-downscale to fit
 * max_w x max_h keeping aspect (never upscales). Returns malloc'd w*h pixels
 * as 0x00RRGGBB, or NULL on failure. */
uint32_t *image_load(const char *path, int max_w, int max_h, uint32_t bg, int *w, int *h);

#endif
