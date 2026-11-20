// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAP_VECTOR_H
#define MAP_VECTOR_H

#define SHORTBREAD_MAX_Z 14
#define VECTOR_CAPACITY (768UL * 1024)

enum { nativePng, nativeProject, nativeVectorUnpack, nativeVectorDraw, nativeVectorLabels,
    nativeIndexLabels, nativeLabelBitmap, nativeIndexScene };

enum {
    vectorRoads = 1u << 0, vectorBuildings = 1u << 1,
    vectorWater = 1u << 2, vectorLand = 1u << 3,
    vectorStreetNames = 1u << 4, vectorLandmarks = 1u << 5,
    vectorAngledNames = 1u << 6, vectorHighContrast = 1u << 7,
    vectorCityNames = 1u << 8, vectorWaterNames = 1u << 9,
    vectorProvincialBorders = 1u << 10, vectorNationalBorders = 1u << 11
};

#define VECTOR_NEW_OPTIONS (vectorCityNames | vectorWaterNames | \
    vectorProvincialBorders | vectorNationalBorders)
#define VECTOR_DEFAULT (vectorRoads | vectorBuildings | vectorWater | vectorLand | \
    vectorStreetNames | vectorLandmarks | vectorAngledNames | VECTOR_NEW_OPTIONS)
#define VECTOR_OPTION_MASK (VECTOR_DEFAULT | vectorHighContrast)
#define VECTOR_BORDERS (vectorProvincialBorders | vectorNationalBorders)
#define VECTOR_TILE_OPTIONS (vectorRoads | vectorBuildings | vectorWater | vectorLand | \
    vectorHighContrast | VECTOR_BORDERS)
#define VECTOR_LABEL_OPTIONS (vectorStreetNames | vectorLandmarks | vectorCityNames | vectorWaterNames)

#define VECTOR_LABEL_LIMIT 32
#define VECTOR_AREA_LABEL_LIMIT 8
#define VECTOR_CITY_MIN_Z 4
#define VECTOR_WATER_MIN_Z 5
#define VECTOR_STREET_MIN_Z 10
#define VECTOR_LANDMARK_MIN_Z 14
#define VECTOR_INDEX_CAPACITY (128UL * 1024)
#define VECTOR_SCENE_CAPACITY (192UL * 1024)
#define MAP_GLYPH_HEIGHT 12
#define MAP_GLYPH_COUNT 224
#define MAP_LABEL_BITMAP_SIDE 224

// All fields are bytes so the ARMlet and PACE share the same layout.
enum { labelStreet = 1, labelMainStreet, labelWater, labelLandmark, labelMainLandmark,
    labelTown, labelCity, labelCapital };

typedef struct {
    unsigned char x, y, endX, endY, priority, span;
    char name[54];
} VectorLabel;

typedef struct {
    unsigned char streetCount, landmarkCount, cityCount, waterCount;
    VectorLabel streets[VECTOR_LABEL_LIMIT], landmarks[VECTOR_LABEL_LIMIT];
    VectorLabel cities[VECTOR_AREA_LABEL_LIMIT], waters[VECTOR_AREA_LABEL_LIMIT];
} VectorLabels;

typedef struct {
    unsigned char width, rows[MAP_GLYPH_HEIGHT][2];
} MapGlyph;

#ifndef PALMOS
int VectorIndexLabels(const unsigned char *data, unsigned size, unsigned char *scratch,
    void *index, unsigned options);
void MapLabelBitmap(const MapGlyph *font, const char *text, unsigned char *pixels,
    unsigned width, unsigned height, int ux, int uy, unsigned colors);
void VectorReadLabels(const unsigned char *data, unsigned size, unsigned char *scratch,
    unsigned shift, unsigned childX, unsigned childY, VectorLabels *labels, const void *index,
    unsigned options);
int VectorIndexScene(const unsigned char *data, unsigned size, void *index, unsigned char *scratch);
int VectorDraw(const unsigned char *data, unsigned size,
    unsigned char *const *bands, unsigned char *scratch, unsigned shift,
    unsigned childX, unsigned childY, unsigned options, const void *index);
unsigned VectorUnpack(const unsigned char *data, unsigned size,
    unsigned char *out, unsigned capacity, unsigned char *scratch);
#endif
#endif
