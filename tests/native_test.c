// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../src/vector.h"

int MapDecode(const unsigned char *png, unsigned len, unsigned char *pixels,
              unsigned stride, unsigned char *scratch);
int MapDecodePreview(const unsigned char *png, unsigned len, unsigned char *pixels,
    unsigned stride, unsigned char *scratch, unsigned zoom, unsigned x, unsigned y);
unsigned MapProject(unsigned char *args);

static void be(unsigned char *p, unsigned n)
{ p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }

static unsigned word(const unsigned char *p)
{ return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3]; }

static unsigned add_chunk(unsigned char *png, const char *type, const unsigned char *bits, unsigned n)
{
    be(png,n);
    memcpy(png+4,type,4);
    if (n) memcpy(png+8,bits,n);
    be(png+8+n,(unsigned)crc32(0,png+4,n+4));
    return n+12;
}

static void fixture(unsigned char *bits, unsigned char *scratch)
{
    const unsigned char ihdr[13]={0,0,1,0,0,0,1,0,1,3};
    const unsigned char palette[]={255,0,0,0,255,0};
    unsigned char raw[256*33], zip[2048], png[4096];
    memcpy(png,"\x89PNG\r\n\x1a\n",8);
    unsigned cursor=8;
    cursor+=add_chunk(png+cursor,"IHDR",ihdr,sizeof(ihdr));
    cursor+=add_chunk(png+cursor,"PLTE",palette,sizeof(palette));
    for (unsigned y=0;y<256;++y) {
        unsigned filter=y%5, current=y<64 ? 0xaa : 0x55;
        unsigned up=y==0 ? 0 : y<=64 ? 0xaa : 0x55;
        raw[y*33]=filter;
        for (unsigned x=0;x<32;++x) {
            unsigned left=x ? current : 0;
            // For a constant row, the upper-left sample equals up after
            // the first byte, so Paeth picks left.
            unsigned predictor=filter==0 ? 0 : filter==1 ? left :
                filter==2 ? up : filter==3 ? (left+up)/2 : x ? left : up;
            raw[y*33+1+x]=(unsigned char)(current-predictor);
        }
    }
    unsigned long n=sizeof(zip);
    assert(compress(zip,&n,raw,sizeof(raw))==Z_OK);
    cursor+=add_chunk(png+cursor,"IDAT",zip,(unsigned)n/2);
    cursor+=add_chunk(png+cursor,"IDAT",zip+n/2,(unsigned)(n-n/2));
    cursor+=add_chunk(png+cursor,"IEND",NULL,0);
    assert(MapDecode(png,cursor,bits,512,scratch));
    const unsigned char first[]={7,224,248,0}, second[]={248,0,7,224};
    for (unsigned y=0;y<256;++y) {
        const unsigned char *pair=y<64 ? first : second;
        for (unsigned x=0;x<512;x+=sizeof(first))
            assert(!memcmp(bits+y*512+x,pair,sizeof(first)));
    }
    assert(MapDecodePreview(png,cursor,bits,512,scratch,2,1,0));
    const unsigned char expanded[]={7,224,7,224,7,224,7,224,248,0,248,0,248,0,248,0};
    assert(!memcmp(bits,expanded,sizeof(expanded)) &&
        !memcmp(bits+255*512,expanded,sizeof(expanded)));
    assert(MapDecodePreview(png,cursor,bits,512,scratch,1,1,1));
    const unsigned char lower[]={248,0,248,0,7,224,7,224};
    assert(!memcmp(bits,lower,sizeof(lower)) && !memcmp(bits+255*512,lower,sizeof(lower)));
    assert(!MapDecodePreview(png,cursor,bits,512,scratch,2,4,0));
    png[45]^=1;
    assert(!MapDecode(png,cursor,bits,512,scratch));
    assert(!MapDecode(png,cursor-20,bits,512,scratch));
}

static unsigned pixel(const unsigned char *bits, unsigned x, unsigned y)
{
    const unsigned char *p=bits+y*512+x*2;
    return ((unsigned)p[0]<<8) | p[1];
}

static void vector_fixture(unsigned char *bits, unsigned char *scratch)
{
    // One building fills the tile with a square courtyard in its NW quadrant.
    unsigned char mvt[]={
        0x1a,0x34,0x0a,0x09,'b','u','i','l','d','i','n','g','s',0x12,0x22,
        0x18,0x03,0x22,0x1e,0x09,0x00,0x00,0x1a,0x80,0x40,0x00,0x00,
        0x80,0x40,0xff,0x3f,0x00,0x0f,0x09,0x80,0x10,0xff,0x2f,0x1a,
        0x00,0x80,0x10,0x80,0x10,0x00,0x00,0xff,0x0f,0x0f,0x28,0x80,
        0x20,0x78,0x02
    };
    unsigned char gzip[128], decoded[sizeof(mvt)];
    z_stream z={0};
    assert(deflateInit2(&z,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_in=mvt; z.avail_in=sizeof(mvt);
    z.next_out=gzip; z.avail_out=sizeof(gzip);
    assert(deflate(&z,Z_FINISH)==Z_STREAM_END);
    unsigned length=(unsigned)z.total_out;
    assert(deflateEnd(&z)==Z_OK);
    assert(VectorUnpack(gzip,length,decoded,sizeof(decoded),scratch)==sizeof(mvt));
    assert(!memcmp(mvt,decoded,sizeof(mvt)));
    assert(!VectorUnpack(gzip,length,decoded,sizeof(decoded)-1,scratch));
    gzip[length-8]^=1;
    assert(!VectorUnpack(gzip,length,decoded,sizeof(decoded),scratch));
    assert(VectorUnpack(mvt,sizeof(mvt),decoded,sizeof(decoded),scratch)==sizeof(mvt));

    unsigned char *bands[]={bits,bits+64*512,bits+128*512,bits+192*512};
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,0,0,0,VECTOR_DEFAULT,NULL));
    unsigned building=pixel(bits,0,0), courtyard=pixel(bits,80,80);
    assert(building!=courtyard && pixel(bits,127,127)==courtyard &&
        pixel(bits,128,128)==building && pixel(bits,255,255)==building);
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,1,0,0,VECTOR_DEFAULT,NULL));
    assert(pixel(bits,127,127)==building && pixel(bits,128,128)==courtyard &&
        pixel(bits,255,255)==courtyard);
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,1,1,0,VECTOR_DEFAULT,NULL));
    assert(pixel(bits,0,255)==building);
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,5,31,31,VECTOR_DEFAULT,NULL));
    assert(pixel(bits,0,0)==building && pixel(bits,255,255)==building);
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,0,0,0,VECTOR_DEFAULT & ~vectorBuildings,NULL));
    assert(pixel(bits,0,0)==courtyard && pixel(bits,255,255)==courtyard);
    assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,0,0,0,VECTOR_DEFAULT | vectorHighContrast,NULL));
    assert(pixel(bits,0,0)!=building && pixel(bits,0,0)!=pixel(bits,80,80));
    void *scene=malloc(VECTOR_SCENE_CAPACITY);
    unsigned char *direct=malloc(256*512);
    assert(scene && direct && VectorIndexScene(decoded,sizeof(decoded),scene,scratch));
    for (unsigned y=0;y<4;++y) {
        for (unsigned x=0;x<4;++x) {
            assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,2,x,y,VECTOR_DEFAULT,NULL));
            memcpy(direct,bits,256*512);
            assert(VectorDraw(decoded,sizeof(decoded),bands,scratch,2,x,y,VECTOR_DEFAULT,scene));
            assert(!memcmp(direct,bits,256*512));
        }
    }
    free(direct);
    free(scene);
    assert(!VectorDraw(decoded,sizeof(decoded)-1,bands,scratch,0,0,0,VECTOR_DEFAULT,NULL));
    decoded[19]=0;
    assert(!VectorDraw(decoded,sizeof(decoded),bands,scratch,0,0,0,VECTOR_DEFAULT,NULL));
}

static void label_fixture(unsigned char *scratch)
{
    // A primary street crosses the entire tile. The landmark layer contains
    // a Latin-1 name and a name that needs the English fallback.
    static const unsigned char tile[]={
        0x1a,0x55,0x0a,0x0d,'s','t','r','e','e','t','_','l','a','b','e','l','s',
        0x12,0x13,0x12,0x04,0x00,0x00,0x01,0x01,0x18,0x02,0x22,0x09,0x09,
        0xc7,0x01,0x80,0x10,0x0a,0xd8,0x4f,0x00,
        0x1a,0x04,'k','i','n','d',0x1a,0x04,'n','a','m','e',
        0x22,0x09,0x0a,0x07,'p','r','i','m','a','r','y',
        0x22,0x13,0x0a,0x11,'W','e','l','l','i','n','g','t','o','n',' ','S','t','r','e','e','t',
        0x28,0x80,0x20,0x78,0x02,
        0x1a,0x95,0x01,0x0a,0x04,'p','o','i','s',
        0x12,0x11,0x12,0x06,0x00,0x00,0x01,0x01,0x02,0x02,0x18,0x01,0x22,0x05,0x09,0xf8,0x23,0xb4,0x10,
        0x12,0x0f,0x12,0x04,0x03,0x03,0x01,0x04,0x18,0x01,0x22,0x05,0x09,0x90,0x03,0xb0,0x09,
        0x1a,0x08,'m','a','n','_','m','a','d','e',0x1a,0x04,'n','a','m','e',
        0x1a,0x07,'n','a','m','e','_','e','n',0x1a,0x07,'a','m','e','n','i','t','y',
        0x22,0x07,0x0a,0x05,'t','o','w','e','r',
        0x22,0x0b,0x0a,0x09,0xe5,0x92,0x8c,0xe5,0xb9,0xb3,0xe5,0xa1,0x94,
        0x22,0x0d,0x0a,0x0b,'P','e','a','c','e',' ','T','o','w','e','r',
        0x22,0x12,0x0a,0x10,'p','l','a','c','e','_','o','f','_','w','o','r','s','h','i','p',
        0x22,0x09,0x0a,0x07,0xc3,0x89,'g','l','i','s','e',0x28,0x80,0x20,0x78,0x02
    };
    VectorLabels labels;
    VectorReadLabels(tile,sizeof(tile),scratch,0,0,0,&labels,NULL,VECTOR_DEFAULT);
    assert(labels.streetCount==1 && labels.landmarkCount==2);
    assert(!strcmp(labels.streets[0].name,"Wellington Street") &&
        labels.streets[0].priority==2 && labels.streets[0].span==255);
    assert(!strcmp(labels.landmarks[0].name,"Peace Tower"));
    assert(!strcmp(labels.landmarks[1].name,"\xc9glise"));
    VectorReadLabels(tile,sizeof(tile),scratch,2,2,1,&labels,NULL,VECTOR_DEFAULT);
    assert(labels.streetCount==1 && labels.streets[0].y==0);
    assert(labels.landmarkCount==1 && labels.landmarks[0].x==63 && labels.landmarks[0].y==6);
    void *index=malloc(VECTOR_INDEX_CAPACITY);
    assert(index && VectorIndexLabels(tile,sizeof(tile),scratch,index,VECTOR_DEFAULT));
    VectorLabels indexed;
    VectorReadLabels(tile,sizeof(tile),scratch,2,2,1,&indexed,index,VECTOR_DEFAULT);
    assert(indexed.streetCount==labels.streetCount && indexed.landmarkCount==labels.landmarkCount);
    assert(!memcmp(indexed.streets,labels.streets,sizeof(VectorLabel)));
    assert(!memcmp(indexed.landmarks,labels.landmarks,sizeof(VectorLabel)));
    VectorReadLabels(tile,sizeof(tile),scratch,2,2,2,&labels,index,VECTOR_DEFAULT);
    assert(!labels.streetCount && !labels.landmarkCount);
    free(index);
    VectorReadLabels(tile,3,scratch,0,0,0,&labels,NULL,VECTOR_DEFAULT);
    assert(!labels.streetCount && !labels.landmarkCount);
}

static void geography_fixture(unsigned char *bits, unsigned char *scratch)
{
    // National, provincial and local lines followed by a capital, a lake and a river.
    static const unsigned char tile[] =
        "\032\140\012\012" "boundaries"
        "\022\020\022\002\000\000\030\002\042\010\011\000\200\004\012\200\100\000"
        "\022\020\022\002\000\001\030\002\042\010\011\000\200\010\012\200\100\000"
        "\022\020\022\002\000\002\030\002\042\010\011\000\200\014\012\200\100\000"
        "\032\013" "admin_level" "\042\002\040\002\042\002\050\004\042\002\060\014\050\200\040\170\002"
        "\032\105\012\014" "place_labels"
        "\022\017\022\004\000\000\001\001\030\001\042\005\011\200\010\200\012"
        "\032\004" "name" "\032\004" "kind" "\042\010\012\006" "Ottawa"
        "\042\011\012\007" "capital" "\050\200\040\170\002"
        "\032\077\012\025" "water_polygons_labels"
        "\022\015\022\002\000\000\030\001\042\005\011\200\014\200\014"
        "\032\004" "name" "\042\014\012\012" "Dow's Lake" "\050\200\040\170\002"
        "\032\165\012\022" "water_lines_labels"
        "\022\022\022\004\000\000\001\001\030\002\042\010\011\000\210\016\012\200\100\000"
        "\032\004" "name" "\032\007" "name_en"
        "\042\047\012\045" "Ottawa River / Rivi\303\250re des Outaouais"
        "\042\016\012\014" "Ottawa River" "\050\200\040\170\002";
    const unsigned size=sizeof(tile)-1;
    unsigned char *bands[]={bits,bits+64*512,bits+128*512,bits+192*512};
    void *scene=malloc(VECTOR_SCENE_CAPACITY);
    void *index=malloc(VECTOR_INDEX_CAPACITY);
    assert(scene && index && VectorIndexScene(tile,size,scene,scratch));
    for (unsigned shift=0;shift<=2;shift+=2) {
        for (unsigned indexed=0;indexed<=1;++indexed) {
            const void *geometry=indexed ? scene : NULL;
            unsigned y=16<<shift;
            assert(VectorDraw(tile,size,bands,scratch,shift,0,0,vectorNationalBorders,geometry));
            unsigned paper=pixel(bits,20,0), national=pixel(bits,20,y);
            assert(national!=paper && pixel(bits,20,y+1)==national && pixel(bits,20,y*2)==paper);
            assert(VectorDraw(tile,size,bands,scratch,shift,0,0,vectorProvincialBorders,geometry));
            unsigned province=pixel(bits,20,y*2);
            assert(province!=paper && province!=national && pixel(bits,20,y)==paper);
            assert(pixel(bits,20,y*3)==paper);
        }
    }
    VectorLabels labels;
    VectorReadLabels(tile,size,scratch,0,0,0,&labels,NULL,VECTOR_DEFAULT);
    assert(labels.cityCount==1 && labels.cities[0].priority==labelCapital);
    assert(!strcmp(labels.cities[0].name,"Ottawa"));
    assert(labels.waterCount==2 && !strcmp(labels.waters[0].name,"Dow's Lake"));
    assert(!strcmp(labels.waters[1].name,"Ottawa River") && labels.waters[1].span==255);
    assert(VectorIndexLabels(tile,size,scratch,index,vectorWaterNames));
    VectorReadLabels(tile,size,scratch,2,0,0,&labels,index,vectorWaterNames);
    assert(!labels.cityCount && labels.waterCount==2 && labels.waters[0].x==192);
    VectorReadLabels(tile,size,scratch,0,0,0,&labels,NULL,vectorCityNames);
    assert(labels.cityCount==1 && !labels.waterCount);
    free(index);
    free(scene);
}

static void text_fixture(void)
{
    MapGlyph font[MAP_GLYPH_COUNT]={0};
    font['A'-32].width=5;
    for (unsigned y=3;y<8;++y) font['A'-32].rows[y][0]=0x20;
    unsigned char *pixels=malloc(MAP_LABEL_BITMAP_SIDE*MAP_LABEL_BITMAP_SIDE);
    assert(pixels);
    unsigned stride=MAP_LABEL_BITMAP_SIDE;
    MapLabelBitmap(font,"A",pixels,13,16,256,0,0x7f0102);
    assert(pixels[0]==127 && pixels[7*stride+6]==2 && pixels[7*stride+7]==1);
    MapLabelBitmap(font,"A",pixels,16,13,0,-256,0x7f0102);
    assert(pixels[6*stride+8]==2 && pixels[5*stride+8]==1 && pixels[0]==127);

    // A one-pixel stroke should remain connected at both diagonal bearings.
    MapGlyph *dash=font+'-'-32;
    dash->width=12;
    dash->rows[5][0]=0xff; dash->rows[5][1]=0xf0;
    for (int dir=-1;dir<=1;dir+=2) {
        MapLabelBitmap(font,"-",pixels,22,22,181,dir*181,0x7f0102);
        for (unsigned x=8;x<=13;++x) {
            unsigned ink=0;
            for (unsigned y=0;y<22;++y) {
                unsigned value=pixels[y*stride+x];
                assert(value==127 || value==1 || value==2);
                ink+=value==2;
            }
            assert(ink);
        }
    }
    // The interior of a rotated filled glyph should have no pinholes.
    for (unsigned y=3;y<=8;++y) {
        dash->rows[y][0]=0xff; dash->rows[y][1]=0xf0;
    }
    MapLabelBitmap(font,"-",pixels,22,22,222,128,0x7f0102);
    for (int y=1;y<21;++y) {
        for (int x=1;x<21;++x) {
            int sx=6*256+((2*x-21)*222+(2*y-21)*128)/2;
            int sy=6*256+((2*y-21)*222-(2*x-21)*128)/2;
            if (sx>=256 && sx<11*256 && sy>=4*256 && sy<8*256)
                assert(pixels[y*stride+x]==2);
        }
    }
    free(pixels);
}

int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned char *bits=malloc(256*512), *scratch=malloc(48*1024), png[128*1024];
    assert(bits && scratch);
    fixture(bits,scratch);
    vector_fixture(bits,scratch);
    geography_fixture(bits,scratch);
    label_fixture(scratch);
    text_fixture();
    FILE *f=fopen(argv[1],"rb");
    assert(f);
    unsigned n=(unsigned)fread(png,1,sizeof(png),f);
    fclose(f);
    assert(MapDecode(png,n,bits,512,scratch));
    unsigned char args[12]={0};
    assert(MapProject(args) && word(args+4)==0x08000000 && word(args+8)==0x08000000);
    be(args+4,45600000); be(args+8,(unsigned)-73500000);
    assert(MapProject(args));
    assert(word(args+4)==79412155 && word(args+8)>95000000 && word(args+8)<97000000);
    be(args+4,91000000);
    assert(!MapProject(args));
    free(scratch);
    free(bits);
    puts("PNG palette, malformed PNG, atlas, Mercator, vector overzoom, labels and gzip checks passed");
    return 0;
}
