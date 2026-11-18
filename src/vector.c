// SPDX-License-Identifier: GPL-3.0-or-later
#include <stddef.h>
#include <stdint.h>

#ifdef MAP_HOST_TEST
#include <string.h>
#else
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#endif
#include "vector.h"

enum { side = 256, subpixel = 16, maskStride = side / 8 };
enum { line = 2, polygon = 3 };
typedef unsigned char Byte;
typedef struct { const Byte *at, *end; } Span;

#define RGB565(r,g,b) (((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3))

static const struct {
    char name[16];
    unsigned short color;
    Byte type, width;
} styles[] = {
    {"ocean",           RGB565(166,204,223), polygon, 0},
    {"land",            RGB565(216,232,199), polygon, 0},
    {"sites",           RGB565(226,223,212), polygon, 0},
    {"water_polygons",  RGB565(166,204,223), polygon, 0},
    {"water_lines",     RGB565(148,193,217), line, 1},
    {"street_polygons", RGB565(255,252,237), polygon, 0},
    {"streets",         RGB565(255,253,247), line, 2},
    {"bridges",         RGB565(255,253,247), polygon, 0},
    {"buildings",       RGB565(181,143,130), polygon, 0},
};
enum { styleCount = sizeof(styles) / sizeof(*styles) };

typedef struct {
    Byte *const *bands;
    Byte *mask;
    unsigned short color;
    int firstRow, lastRow;
    unsigned width;
} Canvas;

typedef struct {
    const Byte *data;
    unsigned size, number, wire;
} Field;

static int varint(Span *s, unsigned *out)
{
    unsigned value = 0;
    for (unsigned shift = 0; shift < 32 && s->at < s->end; shift += 7) {
        unsigned b = *s->at++;
        if (shift == 28 && b > 15) return 0;
        value |= (b & 127) << shift;
        if (!(b & 128)) { *out = value; return 1; }
    }
    return 0;
}

static int field(Span *s, Field *f)
{
    unsigned tag;
    if (!varint(s, &tag) || !(tag >> 3)) return 0;
    f->number = tag >> 3;
    f->wire = tag & 7;
    f->data = s->at;
    switch (f->wire) {
    case 0:
        // Ignored fields can contain a 64-bit feature ID.
        for (unsigned i = 0; i < 10 && s->at < s->end; ++i)
            if (*s->at++ < 128) {
                f->size = (unsigned)(s->at - f->data);
                return 1;
            }
        return 0;
    case 1: f->size = 8; break;
    case 2:
        if (!varint(s, &f->size)) return 0;
        f->data = s->at;
        break;
    case 5: f->size = 4; break;
    default: return 0;
    }
    if (f->size > (unsigned)(s->end - s->at)) return 0;
    s->at += f->size;
    return 1;
}

static Span payload(Field f) { return (Span){f.data, f.data + f.size}; }

typedef uint16_t Pixel __attribute__((may_alias));
typedef uint32_t PixelPair __attribute__((may_alias));

static void paint(Canvas *c, int y, int from, int to)
{
    Byte *row = c->bands[y >> 6] + ((y & 63) << 9);
    Pixel *p = (Pixel *)(row + from * 2);
    Pixel *end = (Pixel *)(row + to * 2);
    unsigned color = c->color;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    color = ((color & 255) << 8) | (color >> 8);
#endif
    if ((uintptr_t)p & 2u && p < end) *p++ = (Pixel)color;
    uint32_t pair = (color << 16) | color;
    while (end - p >= 2) {
        *(PixelPair *)p = pair;
        p += 2;
    }
    if (p < end) *p = (Pixel)color;
}

// First pixel whose center is on or beyond the fixed-point position.
static int pixelCeil(int p) { return (p + 7) >> 4; }

static int scaledDiv(int a, int b, int d)
{
    long long numerator = (long long)a * b;
    if (numerator == (int)numerator) return (int)numerator / d;
    return (int)(numerator / d);
}

static void crossing(Canvas *c, int x0, int y0, int x1, int y1)
{
    if (y1 < y0) {
        int swap = y0; y0 = y1; y1 = swap;
        swap = x0; x0 = x1; x1 = swap;
    }
    int start = pixelCeil(y0), end = pixelCeil(y1);
    if (start < 0) start = 0;
    if (end > side) end = side;
    if (start >= end || (x0 >= side * subpixel && x1 >= side * subpixel)) return;

    if (start < c->firstRow) c->firstRow = start;
    if (end > c->lastRow) c->lastRow = end;
    if (x0 <= 8 && x1 <= 8) {
        for (int y = start; y < end; ++y) c->mask[y * maskStride] ^= 1;
        return;
    }
    int dy = y1 - y0, dx = x1 - x0;
    // The short edges common at source zoom use a single 32-bit division.
    int step = end - start > 1 && dx ? scaledDiv(dx, subpixel * 256, dy) : 0;
    int x = x0 * 256;
    if (dx) x += scaledDiv(dx, (start * subpixel + 8 - y0) * 256, dy);
    for (int y = start; y < end; ++y) {
        int col = pixelCeil(x >> 8);
        if (col < 0) col = 0;
        if (col < side) c->mask[y * maskStride + col / 8] ^= 1u << (col % 8);
        if (y + 1 < end) x += step;
    }
}

static void fillPolygon(Canvas *c)
{
    for (int y = c->firstRow; y < c->lastRow; ++y) {
        Byte *row = c->mask + y * maskStride;
        int start = -1;
        for (int i = 0; i < maskStride; ++i) {
            unsigned bits = row[i];
            row[i] = 0;
            for (int bit = 0; bits; ++bit, bits >>= 1) {
                if (!(bits & 1)) continue;
                int x = i * 8 + bit;
                if (start < 0) start = x;
                else {
                    paint(c, y, start, x);
                    start = -1;
                }
            }
        }
        if (start >= 0) paint(c, y, start, side);
    }
}

static unsigned outcode(int x, int y, int low, int high)
{
    return (x < low ? 1u : x > high ? 2u : 0u) |
           (y < low ? 4u : y > high ? 8u : 0u);
}

static int clipSegment(int *left, int *top, int *right, int *bottom, int low, int high)
{
    int x0 = *left, y0 = *top, x1 = *right, y1 = *bottom;
    for (;;) {
        unsigned a = outcode(x0, y0, low, high), b = outcode(x1, y1, low, high);
        if (a & b) return 0;
        if (!(a | b)) break;
        if (!a) {
            int swap = x0; x0 = x1; x1 = swap;
            swap = y0; y0 = y1; y1 = swap;
            a = b;
        }
        if (a & 12) {
            int y = a & 4 ? low : high;
            x0 += (int)((long long)(x1 - x0) * (y - y0) / (y1 - y0));
            y0 = y;
        } else {
            int x = a & 1 ? low : high;
            y0 += (int)((long long)(y1 - y0) * (x - x0) / (x1 - x0));
            x0 = x;
        }
    }
    *left = x0; *top = y0; *right = x1; *bottom = y1;
    return 1;
}

static void stroke(Canvas *c, int x0, int y0, int x1, int y1)
{
    x0 >>= 4; y0 >>= 4; x1 >>= 4; y1 >>= 4;
    if (!clipSegment(&x0, &y0, &x1, &y1, 1 - (int)c->width, side - 1)) return;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int dx = (x1 - x0) * sx, dy = (y1 - y0) * sy, error = dx - dy;
    for (;;) {
        int left = x0 < 0 ? 0 : x0, right = x0 + (int)c->width;
        if (right > side) right = side;
        for (int y = y0; y < y0 + (int)c->width && y < side; ++y)
            if (y >= 0) paint(c, y, left, right);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice > -dy) { error -= dy; x0 += sx; }
        if (twice < dx) { error += dx; y0 += sy; }
    }
}

static int zigzag(unsigned v) { return (int)(v >> 1) ^ -(int)(v & 1); }

static int readDelta(Span *s, int *x, int *y, int extent)
{
    unsigned vx, vy;
    if (!varint(s, &vx) || !varint(s, &vy)) return 0;
    int dx = zigzag(vx), dy = zigzag(vy);
    // Bound intermediate math while allowing the tile's buffer geometry.
    if (dx < -8 * extent - *x || dx > 8 * extent - *x ||
        dy < -8 * extent - *y || dy > 8 * extent - *y) return 0;
    *x += dx;
    *y += dy;
    return 1;
}

static int geometry(Canvas *c, Span s, unsigned type, int extent,
    unsigned shift, unsigned childX, unsigned childY)
{
    int x = 0, y = 0, px = 0, py = 0, startX = 0, startY = 0;
    int offsetX = (int)childX * side * subpixel, offsetY = (int)childY * side * subpixel;
    int scale = (side * subpixel) << shift;
    int points = 0;
    c->firstRow = side;
    c->lastRow = 0;
    while (s.at < s.end) {
        unsigned cmd;
        if (!varint(&s, &cmd)) return 0;
        unsigned op = cmd & 7, count = cmd >> 3;
        if (op == 7 && type == polygon && count == 1 && points >= 3) {
            crossing(c, px, py, startX, startY);
            points = 0;
            continue;
        }
        if (op == 1) {
            if (count != 1 || (type == polygon && points)) return 0;
        } else if (op != 2 || !count || !points) return 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!readDelta(&s, &x, &y, extent)) return 0;
            int nx, ny;
            if (extent == 4096) {
                nx = x * (1 << shift) - offsetX;
                ny = y * (1 << shift) - offsetY;
            } else {
                nx = (int)((long long)x * scale / extent) - offsetX;
                ny = (int)((long long)y * scale / extent) - offsetY;
            }
            if (op == 1) {
                startX = nx; startY = ny; points = 1;
            } else {
                if (type == polygon) crossing(c, px, py, nx, ny);
                else stroke(c, px, py, nx, ny);
                ++points;
            }
            px = nx; py = ny;
        }
    }
    if (type == polygon) {
        if (points) return 0;
        fillPolygon(c);
    }
    return 1;
}

static int renderLayer(Canvas *c, Span layer, unsigned style,
    unsigned shift, unsigned childX, unsigned childY)
{
    unsigned extent = 4096;
    for (Span fields = layer; fields.at < fields.end;) {
        Field f;
        if (!field(&fields, &f)) return 0;
        if (f.number == 5 && f.wire == 0) {
            Span value = payload(f);
            if (!varint(&value, &extent) || !extent || extent > 65536) return 0;
        }
    }
    c->color = styles[style].color;
    c->width = styles[style].width + (styles[style].width && shift >= 3 ? 1 : 0);
    while (layer.at < layer.end) {
        Field f;
        if (!field(&layer, &f)) return 0;
        if (f.number != 2 || f.wire != 2) continue;
        Span feature = payload(f), commands = {0};
        unsigned type = 0;
        while (feature.at < feature.end) {
            if (!field(&feature, &f)) return 0;
            if (f.number == 4 && f.wire == 2) commands = payload(f);
            if (f.number == 3 && f.wire == 0) {
                Span value = payload(f);
                if (!varint(&value, &type)) return 0;
            }
        }
        if (type == styles[style].type && commands.at &&
            !geometry(c, commands, type, (int)extent, shift, childX, childY)) return 0;
    }
    return 1;
}

int VectorDraw(const Byte *data, unsigned size, Byte *const *bands, Byte *scratch,
    unsigned shift, unsigned childX, unsigned childY)
{
    if (shift > 5 || childX >= (1u << shift) || childY >= (1u << shift)) return 0;
    Span tile = {data, data + size}, layers[styleCount] = {{0}};
    while (tile.at < tile.end) {
        Field f;
        if (!field(&tile, &f)) return 0;
        if (f.number != 3 || f.wire != 2) continue;
        Span layer = payload(f), header = layer;
        while (header.at < header.end) {
            if (!field(&header, &f)) return 0;
            if (f.number != 1 || f.wire != 2) continue;
            for (unsigned i = 0; i < styleCount; ++i) {
                if (f.size < sizeof(styles[i].name) && !styles[i].name[f.size] &&
                    !memcmp(styles[i].name, f.data, f.size)) layers[i] = layer;
            }
            break;
        }
    }
    Canvas canvas = {bands, scratch, RGB565(239,235,219), side, 0, 0};
    memset(scratch, 0, side * maskStride);
    for (int y = 0; y < side; ++y) paint(&canvas, y, 0, side);
    for (unsigned i = 0; i < styleCount; ++i)
        if (layers[i].at && !renderLayer(&canvas, layers[i], i, shift, childX, childY))
            return 0;
    return 1;
}

// Index only the values in the layer being read. Offsets are relative to
// the layer, so the table fits in the existing ARM scratch allocation.
enum { keyLimit = 128, valueLimit = 8192 };
enum { unusedKey, localName, englishName, roadKind, historic, tourism, manMade, amenity };

typedef struct {
    Span bytes;
    unsigned *values, count;
    unsigned extent;
    Byte keys[keyLimit];
} LabelIndex;

typedef struct { int x, y, endX, endY, span; } Anchor;

static int isText(Span s, const char *name, unsigned len)
{
    return s.at && (unsigned)(s.end - s.at) == len && !memcmp(s.at, name, len);
}
#define IS(s,text) isText(s, text, sizeof(text) - 1)

static Byte labelKey(Span key, int landmarks)
{
    if (IS(key, "name")) return localName;
    if (IS(key, "name_en") || IS(key, "name:en")) return englishName;
    if (!landmarks) return IS(key, "kind") ? roadKind : unusedKey;
    if (IS(key, "historic")) return historic;
    if (IS(key, "tourism")) return tourism;
    if (IS(key, "man_made")) return manMade;
    if (IS(key, "amenity")) return amenity;
    return unusedKey;
}

static int indexLabels(LabelIndex *idx, Span layer, unsigned *values, int landmarks)
{
    *idx = (LabelIndex){.bytes = layer, .values = values, .extent = 4096};
    unsigned keys = 0;
    while (layer.at < layer.end) {
        const Byte *fieldStart = layer.at;
        Field f;
        if (!field(&layer, &f)) return 0;
        if (f.number == 3 && f.wire == 2 && keys < keyLimit)
            idx->keys[keys++] = labelKey(payload(f), landmarks);
        if (f.number == 4 && f.wire == 2 && idx->count < valueLimit)
            values[idx->count++] = (unsigned)(fieldStart - idx->bytes.at);
        if (f.number == 5 && f.wire == 0) {
            Span extent = payload(f);
            if (!varint(&extent, &idx->extent) || !idx->extent || idx->extent > 65536)
                return 0;
        }
    }
    return 1;
}

static Span labelValue(const LabelIndex *idx, unsigned value)
{
    if (value < idx->count) {
        // A string Value starts with field 1. Read its length within the
        // enclosing Value message, even when adjacent messages are valid.
        Span s = {idx->bytes.at + idx->values[value], idx->bytes.end};
        Field f;
        if (field(&s, &f) && f.number == 4 && f.wire == 2) {
            s = payload(f);
            if (field(&s, &f) && f.number == 1 && f.wire == 2) return payload(f);
        }
    }
    return (Span){0};
}

static unsigned priority(Byte key, Span value)
{
    if (key == roadKind) {
        if (IS(value, "motorway") || IS(value, "trunk") || IS(value, "primary")) return 2;
        return 1;
    }
    if (key == manMade && (IS(value, "tower") || IS(value, "lighthouse"))) return 5;
    if (key == amenity && (IS(value, "library") || IS(value, "townhall") ||
        IS(value, "courthouse") || IS(value, "place_of_worship"))) return 5;
    if (key == tourism && (IS(value, "museum") || IS(value, "gallery") ||
        IS(value, "attraction"))) return 5;
    if (key == historic && (IS(value, "castle") || IS(value, "monument") ||
        IS(value, "archaeological_site"))) return 5;
    if ((key == historic && value.at) ||
        (key == tourism && (IS(value, "artwork") || IS(value, "viewpoint")))) return 4;
    return 0;
}

static int copyLabel(char *out, Span name)
{
    if (!name.at) return 0;
    unsigned n = 0;
    while (name.at < name.end && n < sizeof(((VectorLabel *)0)->name) - 1) {
        unsigned ch = *name.at++;
        if (ch >= 0xc2 && ch <= 0xc3 && name.at < name.end && (*name.at & 0xc0) == 0x80) {
            ch = ((ch & 31) << 6) | (*name.at++ & 63);
            if (ch < 0xa0) return 0;
        } else if (ch == 0xe2 && name.end - name.at >= 2 && name.at[0] == 0x80) {
            unsigned tail = name.at[1];
            if (tail == 0x98 || tail == 0x99) ch = '\'';
            else if (tail == 0x93 || tail == 0x94) ch = '-';
            else return 0;
            name.at += 2;
        } else if (ch < 32 || ch >= 127) return 0;
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return n && name.at == name.end;
}

static int labelPixel(int coord, unsigned shift, int extent, unsigned child)
{
    int pixel = extent == 4096 ? (coord * (1 << shift)) >> 4 :
        (int)((long long)coord * (side << shift) / extent);
    return pixel - (int)child * side;
}

static Anchor labelAnchor(Span commands, unsigned type, int extent,
    unsigned shift, unsigned childX, unsigned childY)
{
    Anchor best = {0, 0, 0, 0, -1};
    int x = 0, y = 0, prevX = 0, prevY = 0, started = 0;
    while (commands.at < commands.end) {
        unsigned cmd;
        if (!varint(&commands, &cmd)) break;
        unsigned op = cmd & 7, count = cmd >> 3;
        if (!count || (op != 1 && (op != 2 || !started || type != line))) break;
        for (unsigned i = 0; i < count; ++i) {
            if (!readDelta(&commands, &x, &y, extent)) return best;
            int px = labelPixel(x, shift, extent, childX);
            int py = labelPixel(y, shift, extent, childY);
            if (type == 1 && px >= 0 && px < side && py >= 0 && py < side)
                return (Anchor){px, py, px, py, 0};
            if (type == line && op == 2) {
                int ax = prevX, ay = prevY, bx = px, by = py;
                if (clipSegment(&ax, &ay, &bx, &by, 0, side - 1)) {
                    int dx = ax > bx ? ax - bx : bx - ax, dy = ay > by ? ay - by : by - ay;
                    int span = dx > dy ? dx : dy;
                    if (span >= 8 && span > best.span) best = (Anchor){ax, ay, bx, by, span};
                }
            }
            prevX = px; prevY = py; started = 1;
        }
    }
    return best;
}

static int sameLabel(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void keepLabel(VectorLabel *out, Byte *count, const VectorLabel *label)
{
    unsigned score = label->priority * 256u + label->span;
    unsigned weakest = 0, weakestScore = ~0u;
    for (unsigned i = 0; i < *count; ++i) {
        unsigned old = out[i].priority * 256u + out[i].span;
        // Keep separated segments of the same road as alternative anchors.
        if (sameLabel(out[i].name, label->name) &&
            (int)out[i].x - label->x < 32 && (int)label->x - out[i].x < 32 &&
            (int)out[i].y - label->y < 32 && (int)label->y - out[i].y < 32) {
            if (score > old) out[i] = *label;
            return;
        }
        if (old < weakestScore) { weakest = i; weakestScore = old; }
    }
    if (*count < VECTOR_LABEL_LIMIT) out[(*count)++] = *label;
    else if (score > weakestScore) out[weakest] = *label;
}

enum { sourceLimit = 4095 };
typedef struct {
    unsigned commands, length, name, english, extent;
    unsigned short nameLength, englishLength;
    Byte type, rank, left, top, right, bottom;
} SourceLabel;

typedef struct { unsigned count; SourceLabel entry[sourceLimit]; } SourceLabels;
_Static_assert(sizeof(SourceLabels)<=VECTOR_INDEX_CAPACITY, "label index must fit");

static void emitLabel(VectorLabels *labels, Anchor a, unsigned rank, Span name, Span english)
{
    VectorLabel label = {a.x, a.y, a.endX, a.endY, rank, a.span, {0}};
    if (!copyLabel(label.name, name) && !copyLabel(label.name, english)) return;
    if (rank >= 4) keepLabel(labels->landmarks, &labels->landmarkCount, &label);
    else keepLabel(labels->streets, &labels->streetCount, &label);
}

static void addSourceLabel(SourceLabels *source, const Byte *base, Span commands,
    int extent, unsigned type, unsigned rank, Span name, Span english)
{
    int x = 0, y = 0, left = 32, top = 32, right = -1, bottom = -1;
    Span cursor = commands;
    while (cursor.at < cursor.end) {
        unsigned cmd;
        if (!varint(&cursor, &cmd) || ((cmd & 7) != 1 && (cmd & 7) != 2)) return;
        for (unsigned i = 0; i < cmd >> 3; ++i) {
            if (!readDelta(&cursor, &x, &y, extent)) return;
            // A cell is one display tile at zoom 19.
            int col = labelPixel(x, 5, extent, 0) >> 8, row = labelPixel(y, 5, extent, 0) >> 8;
            if (col < left) left = col;
            if (col > right) right = col;
            if (row < top) top = row;
            if (row > bottom) bottom = row;
        }
    }
    if (left > 31 || top > 31 || right < 0 || bottom < 0) return;
    if (source->count == sourceLimit) { ++source->count; return; }
    source->entry[source->count++] = (SourceLabel){
        .commands = (unsigned)(commands.at - base), .length = (unsigned)(commands.end - commands.at),
        .name = name.at ? (unsigned)(name.at - base) : 0,
        .english = english.at ? (unsigned)(english.at - base) : 0,
        .extent = (unsigned)extent,
        .nameLength = name.at && name.end - name.at <= 255 ? (unsigned short)(name.end - name.at) : 0,
        .englishLength = english.at && english.end - english.at <= 255 ?
            (unsigned short)(english.end - english.at) : 0,
        .type = type, .rank = rank, .left = left < 0 ? 0 : left, .top = top < 0 ? 0 : top,
        .right = right > 31 ? 31 : right, .bottom = bottom > 31 ? 31 : bottom
    };
}

static void childLabels(const Byte *data, const SourceLabels *source, unsigned shift,
    unsigned childX, unsigned childY, VectorLabels *labels)
{
    for (unsigned i = 0; i < source->count; ++i) {
        const SourceLabel *e = source->entry+i;
        unsigned scale = 5 - shift;
        if (((unsigned)e->left >> scale) > childX || ((unsigned)e->right >> scale) < childX ||
            ((unsigned)e->top >> scale) > childY || ((unsigned)e->bottom >> scale) < childY) continue;
        Span geometry = {data + e->commands, data + e->commands + e->length};
        Anchor a = labelAnchor(geometry,e->type,(int)e->extent,shift,childX,childY);
        if (a.span < 0) continue;
        Span name = {0}, english = {0};
        if (e->nameLength) name = (Span){data + e->name, data + e->name + e->nameLength};
        if (e->englishLength) english = (Span){data + e->english, data + e->english + e->englishLength};
        emitLabel(labels,a,e->rank,name,english);
    }
}

static void readLabelLayer(Span layer, unsigned *scratch, unsigned shift,
    unsigned childX, unsigned childY, int landmarks, VectorLabels *labels,
    const Byte *base, SourceLabels *source)
{
    LabelIndex idx;
    if (!indexLabels(&idx, layer, scratch, landmarks)) return;
    while (layer.at < layer.end) {
        Field f;
        if (!field(&layer, &f)) return;
        if (f.number != 2 || f.wire != 2) continue;
        Span feature = payload(f), tags = {0}, commands = {0};
        unsigned type = 0;
        while (feature.at < feature.end) {
            if (!field(&feature, &f)) return;
            if (f.number == 2 && f.wire == 2) tags = payload(f);
            if (f.number == 4 && f.wire == 2) commands = payload(f);
            if (f.number == 3 && f.wire == 0) {
                Span value = payload(f);
                if (!varint(&value, &type)) return;
            }
        }
        if (!tags.at || !commands.at || (landmarks ? type != 1 : (type != line && type != 1)))
            continue;

        Span name = {0}, english = {0};
        unsigned rank = landmarks ? 0 : 1;
        while (tags.at < tags.end) {
            unsigned key, value;
            if (!varint(&tags, &key) || !varint(&tags, &value)) break;
            if (key >= keyLimit || !idx.keys[key]) continue;
            Byte kind = idx.keys[key];
            Span text = labelValue(&idx, value);
            if (kind == localName) name = text;
            else if (kind == englishName) english = text;
            else {
                unsigned p = priority(kind, text);
                if (p > rank) rank = p;
            }
        }
        if (!rank || (!name.at && !english.at)) continue;
        if (source) {
            addSourceLabel(source,base,commands,idx.extent,type,rank,name,english);
            if (source->count>sourceLimit) return;
        } else {
            Anchor anchor = labelAnchor(commands,type,idx.extent,shift,childX,childY);
            if (anchor.span < 0) continue;
            emitLabel(labels,anchor,rank,name,english);
        }
    }
}

static void scanLabelLayers(const Byte *data, unsigned size, Byte *scratch,
    unsigned shift, unsigned childX, unsigned childY, VectorLabels *labels, SourceLabels *source)
{
    Span tile = {data, data + size};
    while (tile.at < tile.end) {
        Field f;
        if (!field(&tile, &f)) return;
        if (f.number != 3 || f.wire != 2) continue;
        Span layer = payload(f), header = layer;
        while (header.at < header.end) {
            if (!field(&header, &f)) break;
            if (f.number != 1 || f.wire != 2) continue;
            Span name = payload(f);
            int landmarks = IS(name, "pois");
            if (landmarks || IS(name, "street_labels") || IS(name, "streets_polygons_labels"))
                readLabelLayer(layer,(unsigned *)scratch,shift,childX,childY,landmarks,labels,data,source);
            if (source && source->count>sourceLimit) return;
            break;
        }
    }
}

int VectorIndexLabels(const Byte *data, unsigned size, Byte *scratch, void *storage)
{
    SourceLabels *source = storage;
    source->count = 0;
    scanLabelLayers(data,size,scratch,0,0,0,NULL,source);
    return source->count <= sourceLimit;
}

void VectorReadLabels(const Byte *data, unsigned size, Byte *scratch,
    unsigned shift, unsigned childX, unsigned childY, VectorLabels *labels, const void *storage)
{
    labels->streetCount = labels->landmarkCount = 0;
    if (shift > 5 || childX >= (1u << shift) || childY >= (1u << shift)) return;
    if (storage) childLabels(data,storage,shift,childX,childY,labels);
    else scanLabelLayers(data,size,scratch,shift,childX,childY,labels,NULL);
}
