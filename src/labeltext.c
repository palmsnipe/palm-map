// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector.h"

typedef unsigned char Byte;

// A label that fits the 224 px bitmap in both directions can be up to
// 311 px long before rotation. Pack its original, unrotated glyphs.
enum { maxAdvance = 320, maskStride = maxAdvance / 8 };
typedef struct {
    Byte rows[MAP_GLYPH_HEIGHT][maskStride];
    int advance;
} TextMask;

static int makeMask(TextMask *mask, const MapGlyph *font, const char *text)
{
    for (; *text; ++text) {
        Byte ch = (Byte)*text;
        const MapGlyph *g = &font[ch >= 32 ? ch - 32 : 0];
        int pen = mask->advance;
        if (g->width > 16 || pen + g->width > maxAdvance) return 0;
        for (unsigned y = 0; y < MAP_GLYPH_HEIGHT; ++y) {
            unsigned bits = (unsigned)g->rows[y][0] << 8 | g->rows[y][1];
            if (!bits) continue;
            for (unsigned x = 0; x < g->width; ++x) {
                if (!(bits & (32768u >> x))) continue;
                unsigned column = (unsigned)pen + x;
                mask->rows[y][column >> 3] |= 128u >> (column & 7);
            }
        }
        mask->advance += g->width;
    }
    return mask->advance > 0;
}

static unsigned sample(const TextMask *mask, int x, int y)
{
    unsigned col = (unsigned)(x >> 8), row = (unsigned)(y >> 8);
    if (col >= (unsigned)mask->advance || row >= MAP_GLYPH_HEIGHT) return 0;
    return (mask->rows[row][col >> 3] >> (7 - (col & 7))) & 1u;
}

static int covered(const TextMask *mask, int x, int y, int a, int b)
{
    if (sample(mask,x,y)) return 1;
    // A rotated one-pixel stroke can straddle the pixel center. Check the
    // four quadrant centers and paint either solid ink or no ink.
    return sample(mask,x+a,y+b) + sample(mask,x-a,y-b) +
        sample(mask,x+b,y-a) + sample(mask,x-b,y+a) >= 2;
}

static void haloPixel(Byte *p, Byte ink, Byte halo)
{
    *p = ink;
    if (p[-1] != ink) p[-1] = halo;
    if (p[1] != ink) p[1] = halo;
    if (p[-MAP_LABEL_BITMAP_SIDE] != ink) p[-MAP_LABEL_BITMAP_SIDE] = halo;
    if (p[MAP_LABEL_BITMAP_SIDE] != ink) p[MAP_LABEL_BITMAP_SIDE] = halo;
}

static void drawText(const TextMask *mask, Byte *pixels, int width, int height,
    int ux, int uy, Byte ink, Byte halo)
{
    // Scan the narrow strip taken up by the rotated letters. Step along
    // whichever screen axis is closer to the baseline.
    int absY = uy < 0 ? -uy : uy;
    int vertical = absY > ux;
    int along = vertical ? height : width, across = vertical ? width : height;
    int slope = vertical ? ux * 256 / uy : uy * 256 / ux;
    int center = across * 128 + (3 - along) * slope / 2;
    int radius = MAP_GLYPH_HEIGHT; // Half the height projected at up to 45 degrees, plus margins.
    int a = (ux + uy) / 4, b = (ux - uy) / 4;
    for (int i = 1; i < along - 1; ++i, center += slope) {
        int middle = center >> 8;
        int from = middle - radius, to = middle + radius;
        if (from < 1) from = 1;
        if (to >= across - 1) to = across - 2;
        if (from > to) continue;
        int x = vertical ? from : i, y = vertical ? i : from;
        int dx = 2*x + 1 - width, dy = 2*y + 1 - height;
        int sx = mask->advance * 128 + (dx*ux + dy*uy) / 2;
        int sy = MAP_GLYPH_HEIGHT * 128 + (dy*ux - dx*uy) / 2;
        int stepX = vertical ? ux : uy, stepY = vertical ? -uy : ux;
        int stride = vertical ? 1 : MAP_LABEL_BITMAP_SIDE;
        Byte *p = pixels + y * MAP_LABEL_BITMAP_SIDE + x;
        for (int j = from; j <= to; ++j, p += stride, sx += stepX, sy += stepY) {
            if (uy ? covered(mask,sx,sy,a,b) : sample(mask,sx,sy)!=0)
                haloPixel(p,ink,halo);
        }
    }
}

void MapLabelBitmap(const MapGlyph *font, const char *text, Byte *pixels,
    unsigned width, unsigned height, int ux, int uy, unsigned colors)
{
    if (!width || width > MAP_LABEL_BITMAP_SIDE || !height || height > MAP_LABEL_BITMAP_SIDE)
        return;
    Byte transparent = (Byte)(colors >> 16);
    for (unsigned y = 0; y < height; ++y) {
        Byte *p = pixels + y * MAP_LABEL_BITMAP_SIDE;
        for (unsigned x = 0; x < width; ++x) p[x] = transparent;
    }
    TextMask mask = {0};
    if ((ux || uy) && makeMask(&mask,font,text))
        drawText(&mask,pixels,(int)width,(int)height,ux,uy,(Byte)colors,(Byte)(colors >> 8));
}
