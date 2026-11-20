// SPDX-License-Identifier: GPL-3.0-or-later
#include "zlib.h"
#include "vector.h"

typedef unsigned char Byte;
typedef unsigned int Word;
typedef struct {
    Byte *next;
    Word left;
} Arena;

#define SCRATCH_BYTES (48u * 1024)
#define SIDE 256u

static Word be32(const Byte *p)
{
    return (Word)p[0] << 24 | (Word)p[1] << 16 | (Word)p[2] << 8 | p[3];
}

static void put32(Byte *p, Word x)
{
    p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; p[3] = x;
}

#ifdef MAP_HOST_TEST
#include <string.h>
#else
void *memset(void *dst, int val, unsigned size)
{
    Byte *p = dst;
    while (size--) *p++ = (Byte)val;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned size)
{
    Byte *p = dst;
    const Byte *s = src;
    while (size--) *p++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned size)
{
    const Byte *x = a, *y = b;
    while (size--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

#endif

static voidpf alloc(voidpf opaque, uInt count, uInt size)
{
    Arena *a = opaque;
    if (!size || count > a->left / size) return 0;
    Word n = (count * size + 7) & ~7u;
    if (n > a->left) return 0;
    Byte *p = a->next;
    a->next += n;
    a->left -= n;
    return p;
}

static void release(voidpf unused, voidpf ptr)
{
    (void)unused; (void)ptr;
}

static Word crc(const Byte *p, Word size)
{
    static const Word nibble[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    Word v = ~0u;
    while (size--) {
        v ^= *p++;
        v = (v >> 4) ^ nibble[v & 15];
        v = (v >> 4) ^ nibble[v & 15];
    }
    return ~v;
}

static Word le32(const Byte *p)
{
    return (Word)p[3] << 24 | (Word)p[2] << 16 | (Word)p[1] << 8 | p[0];
}

Word VectorUnpack(const Byte *data, Word size, Byte *out, Word capacity, Byte *scratch)
{
    if (size < 2 || data[0] != 0x1f || data[1] != 0x8b) {
        if (size > capacity) return 0;
        memcpy(out, data, size);
        return size;
    }
    if (size < 18 || data[2] != 8 || (data[3] & 0xe0)) return 0;
    const Byte *at = data + 10, *end = data + size - 8;
    unsigned flags = data[3];
    if (flags & 4) {
        if (end - at < 2) return 0;
        unsigned n = at[0] | ((unsigned)at[1] << 8);
        at += 2;
        if (n > (unsigned)(end - at)) return 0;
        at += n;
    }
    for (unsigned bit = 8; bit <= 16; bit <<= 1) {
        if (!(flags & bit)) continue;
        while (at < end && *at) ++at;
        if (at == end) return 0;
        ++at;
    }
    if (flags & 2) {
        if (end - at < 2 || (crc(data, (Word)(at - data)) & 0xffff) !=
            (Word)(at[0] | ((Word)at[1] << 8))) return 0;
        at += 2;
    }
    Word expected = le32(end + 4);
    if (!expected || expected > capacity) return 0;
    Arena arena = {scratch, SCRATCH_BYTES};
    z_stream z = {0};
    z.zalloc = alloc; z.zfree = release; z.opaque = &arena;
    z.next_in = (Byte *)at;
    z.avail_in = (Word)(end - at);
    z.next_out = out;
    z.avail_out = expected;
    if (inflateInit2(&z, -15) != Z_OK) return 0;
    int status = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return status == Z_STREAM_END && !z.avail_in && z.total_out == expected &&
        crc(out, expected) == le32(end) ? expected : 0;
}

typedef struct {
    z_stream z;
    Byte rows[2][1025], palette[256][4];
    unsigned short colors[256];
    Word rowLen, y, stride, kind, depth, channels, ncolors, dark;
    Word cropSide, cropX, cropY, zoom;
    Byte *bands[4];
    int done;
} Png;

static int abs_i(int v) { return v < 0 ? -v : v; }
static int paeth(int a, int b, int c)
{
    int da = abs_i(b-c), db = abs_i(a-c), dc = abs_i(a+b-2*c);
    return da <= db && da <= dc ? a : db <= dc ? b : c;
}

static Word rgb565(Word r, Word g, Word b)
{
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}

static Word renderColor(Word r, Word g, Word b, Word alpha, Word dark)
{
    if (alpha != 255) {
        r = 255 - (255-r)*alpha/255;
        g = 255 - (255-g)*alpha/255;
        b = 255 - (255-b)*alpha/255;
    }
    if (dark) {
        r = 26 + (255-r)*5/8;
        g = 30 + (255-g)*5/8;
        b = 39 + (255-b)*5/8;
    }
    return rgb565(r,g,b);
}

static void prepareColors(Png *p)
{
    if (p->kind == 3) {
        for (Word i = 0; i < p->ncolors; ++i) {
            const Byte *color = p->palette[i];
            p->colors[i] = renderColor(color[0],color[1],color[2],color[3],p->dark);
        }
    } else if (!p->kind) {
        p->ncolors = 1u << p->depth;
        for (Word i = 0; i < p->ncolors; ++i) {
            Word gray = i*255/(p->ncolors-1);
            p->colors[i] = renderColor(gray,gray,gray,255,p->dark);
        }
    }
}

static int unfilter(Byte *row, const Byte *up, Word length, Word bpp, Word filter)
{
    switch (filter) {
    case 0: break;
    case 1:
        for (Word i = bpp; i < length; ++i) row[i] += row[i-bpp];
        break;
    case 2:
        for (Word i = 0; i < length; ++i) row[i] += up[i];
        break;
    case 3:
        for (Word i = 0; i < bpp; ++i) row[i] += up[i] >> 1;
        for (Word i = bpp; i < length; ++i) row[i] += (row[i-bpp] + up[i]) >> 1;
        break;
    case 4:
        for (Word i = 0; i < bpp; ++i) row[i] += up[i];
        for (Word i = bpp; i < length; ++i) row[i] += paeth(row[i-bpp],up[i],up[i-bpp]);
        break;
    default: return 0;
    }
    return 1;
}

static void pixel565(Byte *out, Word color)
{
    out[0] = color >> 8;
    out[1] = color;
}

static Word trueColor(const Png *p, const Byte *row)
{
    Word r = row[0];
    Word g = p->kind == 4 ? r : row[1], b = p->kind == 4 ? r : row[2];
    Word alpha = p->kind == 4 ? row[1] : p->kind == 6 ? row[3] : 255;
    return renderColor(r,g,b,alpha,p->dark);
}

static Byte *outputRow(const Png *p, Word y)
{
    return p->bands[y >> 6] + (y & 63) * p->stride;
}

static int previewRow(Png *p, const Byte *row, Word y)
{
    if (y < p->cropY || y >= p->cropY + p->cropSide) return 1;
    Word displayY = (y-p->cropY) << p->zoom;
    Word scale = 1u << p->zoom, mask = (1u << p->depth) - 1;
    Byte *first = outputRow(p,displayY), *out = first;
    for (Word x=p->cropX; x<p->cropX+p->cropSide; ++x) {
        Word color;
        if (p->channels == 1) {
            Word bit = x*p->depth;
            Word index = (row[bit >> 3] >> (8-p->depth-(bit & 7))) & mask;
            if (index >= p->ncolors) return 0;
            color = p->colors[index];
        } else {
            color = trueColor(p,row+x*p->channels);
        }
        for (Word copy=0; copy<scale; ++copy,out+=2) pixel565(out,color);
    }
    for (Word copy=1; copy<scale; ++copy)
        memcpy(outputRow(p,displayY+copy),first,SIDE*2);
    return 1;
}

static int drawRow(Png *p)
{
    if (p->y >= SIDE) return 0;
    Byte *current = p->rows[p->y & 1], *row = current + 1;
    const Byte *up = p->rows[(p->y & 1) ^ 1] + 1;
    if (!unfilter(row,up,p->rowLen-1,p->channels,current[0])) return 0;
    Word y = p->y++;
    if (p->zoom) return previewRow(p,row,y);
    Byte *out = outputRow(p,y);
    if (p->channels == 1 && p->depth == 8) {
        for (Word x = 0; x < SIDE; ++x, out += 2) {
            if (row[x] >= p->ncolors) return 0;
            pixel565(out,p->colors[row[x]]);
        }
    } else if (p->channels == 1) {
        Word mask = (1u << p->depth) - 1;
        for (Word i = 0; i < p->rowLen-1; ++i) {
            for (int shift = 8 - (int)p->depth; shift >= 0; shift -= (int)p->depth, out += 2) {
                Word index = (row[i] >> shift) & mask;
                if (index >= p->ncolors) return 0;
                pixel565(out,p->colors[index]);
            }
        }
    } else if (p->kind == 2 && !p->dark) {
        for (Word x = 0; x < SIDE; ++x, row += 3, out += 2)
            pixel565(out,rgb565(row[0],row[1],row[2]));
    } else {
        for (Word x = 0; x < SIDE; ++x, row += p->channels, out += 2)
            pixel565(out,trueColor(p,row));
    }
    return 1;
}

static int feed(Png *p, const Byte *data, Word size)
{
    if (p->done) return !size;
    p->z.next_in = (Byte *)data;
    p->z.avail_in = size;
    for (;;) {
        Word previous = p->z.avail_in, available = p->z.avail_out;
        int status = inflate(&p->z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END)
            return 0;
        int full = !p->z.avail_out;
        if (full) {
            if (!drawRow(p)) return 0;
            p->z.next_out = p->rows[p->y & 1];
            p->z.avail_out = p->y == SIDE ? 1 : p->rowLen;
        }
        if (status == Z_STREAM_END) {
            p->done = 1;
            return p->y == SIDE && !p->z.avail_in;
        }
        if (!p->z.avail_in && !full) return 1;
        if (!full && previous == p->z.avail_in && available == p->z.avail_out) return 0;
    }
}

static int decode(const Byte *data, Word size, Byte *const *bands, Word stride, Byte *scratch,
    Word dark, Word zoom, Word childX, Word childY)
{
    if (zoom > 2 || childX >= (1u << zoom) || childY >= (1u << zoom)) return 0;
    if (size < 45 || be32(data) != 0x89504e47 || be32(data+4) != 0x0d0a1a0a ||
        be32(data+8) != 13 || be32(data+12) != 0x49484452 ||
        be32(data+16) != SIDE || be32(data+20) != SIDE || stride < SIDE*2 ||
        data[26] || data[27] || data[28] || crc(data+12,17) != be32(data+29)) return 0;
    Word depth=data[24], kind=data[25], channels;
    switch (kind) {
    case 0: case 3:
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return 0;
        channels=1; break;
    case 2: channels=3; break;
    case 4: channels=2; break;
    case 6: channels=4; break;
    default: return 0;
    }
    if (channels != 1 && depth != 8) return 0;
    Arena arena = {scratch, SCRATCH_BYTES};
    Png *p = alloc(&arena, 1, sizeof(Png));
    if (!p) return 0;
    memset(p, 0, sizeof(*p));
    p->depth=depth; p->channels=channels; p->kind=kind; p->stride=stride;
    p->zoom=zoom; p->cropSide=SIDE >> zoom;
    p->cropX=childX*p->cropSide; p->cropY=childY*p->cropSide;
    memcpy(p->bands,bands,sizeof(p->bands)); p->dark=dark; p->rowLen=1 + (SIDE*depth*channels)/8;
    p->z.next_out=p->rows[0]; p->z.avail_out=p->rowLen;
    p->z.opaque=&arena; p->z.zalloc=alloc; p->z.zfree=release;
    if (inflateInit(&p->z) != Z_OK) return 0;
    int result = 0, idat = 0, gap = 0;
    for (Word i=33; size-i>=12;) {
        Word n=be32(data+i), tag=be32(data+i+4);
        if (n > size-i-12 || crc(data+i+4,n+4) != be32(data+i+8+n)) break;
        const Byte *chunk=data+i+8;
        i += n+12;
        if (tag == 0x49444154) {
            if (gap || (kind==3 && !p->ncolors)) break;
            if (!idat) prepareColors(p);
            if (!feed(p,chunk,n)) break;
            idat=1;
            continue;
        }
        if (idat) gap=1;
        if (tag == 0x504c5445) {
            if (idat || p->ncolors || !n || n>768 || n%3 ||
                (kind==3 && n/3 > (1u << depth))) break;
            p->ncolors=n/3;
            for (Word c=0;c<p->ncolors;c++) {
                memcpy(p->palette[c],chunk+c*3,3);
                p->palette[c][3]=255;
            }
        } else if (tag == 0x74524e53 && kind==3) {
            if (idat || !n || n>p->ncolors) break;
            for (Word c=0;c<n;c++) p->palette[c][3]=chunk[c];
        } else if (tag == 0x49454e44) {
            result=!n && idat && p->done && i==size;
            break;
        } else if (!(tag & 0x20000000)) break;
    }
    inflateEnd(&p->z);
    return result;
}

static unsigned project(Byte *args)
{
    double lat = (int)be32(args+4) / 1000000.0;
    double lon = (int)be32(args+8) / 1000000.0;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return 0;
    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    double a = lat * 0.017453292519943295, a2 = a*a;
    double s = a*(1+a2*(-1.0/6+a2*(1.0/120+a2*(-1.0/5040+
        a2*(1.0/362880+a2*(-1.0/39916800+a2/6227020800.0))))));
    double t=(1+s)/(1-s);
    int e=0;
    while(t>1.4142135623730951) { t*=0.5; ++e; }
    while(t<0.7071067811865475) { t*=2; --e; }
    double d=(t-1)/(t+1), sq=d*d, sum=d;
    for (int n=3;n<=17;n+=2) { d*=sq; sum+=d/n; }
    double y=(0.5-(2*sum+e*0.6931471805599453)/12.566370614359172)*268435456.0;
    if (y<0) y=0;
    if (y>=268435456.0) y=268435455.0;
    put32(args+4, (Word)((lon+180)*268435456.0/360) & 0x0fffffff);
    put32(args+8, (Word)y);
    return 1;
}

#ifdef MAP_HOST_TEST
unsigned MapProject(Byte *args) { return project(args); }
int MapDecode(const Byte *d, Word n, Byte *p, Word stride, Byte *scratch)
{ Byte *bands[4]={p,p+64*stride,p+128*stride,p+192*stride};
    return decode(d,n,bands,stride,scratch,0,0,0,0); }
int MapDecodePreview(const Byte *d, Word n, Byte *p, Word stride, Byte *scratch,
    Word zoom, Word x, Word y)
{ Byte *bands[4]={p,p+64*stride,p+128*stride,p+192*stride};
    return decode(d,n,bands,stride,scratch,0,zoom,x,y); }
#else
Word __attribute__((section(".entry"))) MapNative(void *emu, Byte *args, void *callback)
{
    (void)emu; (void)callback;
    Word command = be32(args);
    if (command == nativeProject) return project(args);
    const Byte *data = (const Byte *)be32(args+4);
    Word size = be32(args+8);
    if (command == nativeVectorUnpack)
        return VectorUnpack(data,size,(Byte *)be32(args+12),be32(args+16),
            (Byte *)be32(args+20));
    if (command == nativeVectorLabels) {
        VectorReadLabels(data,size,(Byte *)be32(args+12),be32(args+16),
            be32(args+20),be32(args+24),(VectorLabels *)be32(args+28),(const void *)be32(args+32),
            be32(args+36));
        return 1;
    }
    if (command == nativeIndexScene)
        return VectorIndexScene(data,size,(void *)be32(args+12),(Byte *)be32(args+16));
    if (command == nativeIndexLabels)
        return VectorIndexLabels(data,size,(Byte *)be32(args+12),(void *)be32(args+16),be32(args+20));
    if (command == nativeLabelBitmap) {
        MapLabelBitmap((const MapGlyph *)data,(const char *)size,(Byte *)be32(args+12),
            be32(args+16),be32(args+20),(int)be32(args+24),(int)be32(args+28),be32(args+32));
        return 1;
    }
    Byte *bands[4], *scratch = (Byte *)be32(args+28);
    for (unsigned i=0;i<4;++i) bands[i]=(Byte *)be32(args+12+i*4);
    if (command == nativeVectorDraw)
        return VectorDraw(data,size,bands,scratch,be32(args+32),be32(args+36),be32(args+40),
            be32(args+44),(const void *)be32(args+48));
    if (command == nativePng)
        return decode(data,size,bands,512,scratch,be32(args+32),be32(args+36),
            be32(args+40),be32(args+44));
    return 0;
}
#endif
