// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int MapDecode(const unsigned char *png, unsigned len, unsigned char *pixels,
              unsigned stride, unsigned char *scratch);
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
        raw[y*33]=0;
        memset(raw+y*33+1,y<64 ? 0xaa : 0x55,32);
    }
    unsigned long n=sizeof(zip);
    assert(compress(zip,&n,raw,sizeof(raw))==Z_OK);
    cursor+=add_chunk(png+cursor,"IDAT",zip,(unsigned)n);
    cursor+=add_chunk(png+cursor,"IEND",NULL,0);
    assert(MapDecode(png,cursor,bits,512,scratch));
    const unsigned char first[]={7,224,248,0}, second[]={248,0,7,224};
    assert(!memcmp(bits,first,sizeof(first)) && !memcmp(bits+64*512,second,sizeof(second)));
    png[45]^=1;
    assert(!MapDecode(png,cursor,bits,512,scratch));
    assert(!MapDecode(png,cursor-20,bits,512,scratch));
}

int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned char *bits=malloc(256*512), *scratch=malloc(48*1024), png[128*1024];
    assert(bits && scratch);
    fixture(bits,scratch);
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
    puts("PNG palette, malformed PNG, atlas and Mercator checks passed");
    return 0;
}
