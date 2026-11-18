// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAP_VECTOR_H
#define MAP_VECTOR_H

#define SHORTBREAD_MAX_Z 14
#define VECTOR_CAPACITY (768UL * 1024)

enum { nativePng, nativeProject, nativeVectorUnpack, nativeVectorDraw, nativeVectorLabels,
    nativeIndexLabels, nativeLabelBitmap };

#define VECTOR_LABEL_LIMIT 32
#define VECTOR_LABEL_MIN_Z 10
#define VECTOR_INDEX_CAPACITY (128UL * 1024)
#define MAP_GLYPH_HEIGHT 12
#define MAP_GLYPH_COUNT 224
#define MAP_LABEL_BITMAP_SIDE 224

// All fields are bytes so the ARMlet and PACE share the same layout.
typedef struct {
    unsigned char x, y, endX, endY, priority, span;
    char name[54];
} VectorLabel;

typedef struct {
    unsigned char streetCount, landmarkCount;
    VectorLabel streets[VECTOR_LABEL_LIMIT], landmarks[VECTOR_LABEL_LIMIT];
} VectorLabels;

typedef struct {
    unsigned char width, rows[MAP_GLYPH_HEIGHT][2];
} MapGlyph;

#ifndef PALMOS
int VectorIndexLabels(const unsigned char *data, unsigned size, unsigned char *scratch,
    void *index);
void MapLabelBitmap(const MapGlyph *font, const char *text, unsigned char *pixels,
    unsigned width, unsigned height, int ux, int uy, unsigned colors);
void VectorReadLabels(const unsigned char *data, unsigned size, unsigned char *scratch,
    unsigned shift, unsigned childX, unsigned childY, VectorLabels *labels, const void *index);
int VectorDraw(const unsigned char *data, unsigned size,
    unsigned char *const *bands, unsigned char *scratch, unsigned shift,
    unsigned childX, unsigned childY);
unsigned VectorUnpack(const unsigned char *data, unsigned size,
    unsigned char *out, unsigned capacity, unsigned char *scratch);
#endif
#endif
