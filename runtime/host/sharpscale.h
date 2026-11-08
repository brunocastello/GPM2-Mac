/* Software "sharp bilinear" scaler (portable, C89): nearest-neighbor pre-scale by k = floor(scale) and
 * then bilinear interpolation to the final size; RGB 0x00RRGGBB output from the 8-bit frame + palette. */
#ifndef GPM2_SHARPSCALE_H
#define GPM2_SHARPSCALE_H
#include <stdint.h>

/* src: sw x sh indices (stride = sw); pal: RGBX (r,g,b,0); dst: tw x th 32-bit pixels, `dstride` in bytes.
 * Returns 0 if memory for the tables ran out. */
int sharp_scale(const uint8_t *src, int sw, int sh, const uint8_t pal[256][4], int tw, int th, uint32_t *dst, int dstride);
#endif
