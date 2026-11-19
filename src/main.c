// SPDX-License-Identifier: GPL-3.0-or-later
#include <PalmOS.h>
#include <PceNativeCall.h>
#include <PenInputMgr.h>
#include "resource.h"
#include "map.h"
#include "memory.h"
#include "vector.h"
#include "net.h"

#define ARM_RESOURCE 1000
#define SLOTS 12
#define SCRATCH_SIZE (48UL*1024)
#define MAX_Z 19
#define OVERVIEW_Z 3
#define TOP 16
#define FOOT 14
#define ZOOM_MARGIN 6
#define ZOOM_WIDTH 24
#define ZOOM_HEIGHT 20
#define ZOOM_GAP 4
#define RESULTS 8
#define VERSION 1
#define HTTPS_PREF 1
#define SHORTBREAD_PREF 2
#define OFFLINE_PREF 3
#define SOURCE_MAX 150
#define OSM "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

#define SHORTBREAD "https://vector.openstreetmap.org/shortbread_v1/{z}/{x}/{y}.mvt"
#define VECTOR_SOURCE 0x26f02d41UL
#define CYCLE "https://a.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png"
#define CYCLE_SOURCE 0x4f51e73aUL

enum { streets, night, overview, custom, shortbread, cycle };
enum { exactTile, overviewFallback, previewParent, previewGrandparent };
typedef struct {
    UInt32 x,y,markX,markY;
    UInt8 z,layer;
    Boolean pin;
    Char name[48], source[SOURCE_MAX+1];
} Pref;

typedef struct {
    TileKey key;
    BitmapType *base[4],*dense[4];
    UInt32 used;
    CacheMeta meta;
    VectorLabels *labels;
    UInt8 layer, fallback;
} Tile;

typedef struct {
    TileKey key;
    Coord x,y;
} Visible;

typedef struct {
    UInt32 x,y;
    Char label[49];
} Place;

typedef struct {
    Pref view;
    UInt16 vectorOptions;
    UInt32 sourceId, useCount, retryAt, settleAt, lastCounter, counterAt, dragFrame;
    MapNet net;
    TileCache cache;
    Tile tiles[SLOTS];
    TileKey loading, vectorKey;
    CacheMeta vectorMeta;
    UInt8 *vectorData;
    UInt32 vectorSize;
    void *labelIndex, *sceneIndex;
    Boolean indexReady, indexTried, sceneReady, sceneTried, labelPainterTried;
    BitmapType *labelBase, *labelBitmap;
    UInt8 *labelPixels, labelKey, labelHalo, labelStreet, labelLandmark;
    MapGlyph *glyphs;
#ifdef MAP_VECTOR_BENCH
    UInt32 unpackTicks, renderTicks, peakTicks, rendered;
    UInt32 indexTicks, extractTicks, overlayTicks;
    UInt16 labelCount;
#endif
    MemHandle arm;
    NativeFuncType *native;
    UInt8 *scratch;
    RectangleType map;
    Coord width,height,tileSide,dragX,dragY;
    Boolean highDensity,drag,dragMoved,dragPending,needWork,blocked,diaLegacy,notify,forceTls12,offline;
    UInt16 card;
    LocalID db;
    Place results[RESULTS];
    Char *choices[RESULTS];
    UInt16 resultCount, searchResource, searchOffset, searchLength;
    UInt8 searchRanks[RESULTS];
    Boolean searching;
    Char query[61];
    const Char *searchStatus;
} App;

static App *State(void)
{
    UInt32 p;
    return FtrGet(CREATOR,0,&p)==errNone ? (App *)p : NULL;
}


static void Destroy(Tile *t)
{
    for (UInt16 i=0;i<4;++i) {
        if (t->dense[i]) BmpDelete(t->dense[i]);
        if (t->base[i]) BmpDelete(t->base[i]);
    }
    if (t->labels) MemPtrFree(t->labels);
    MemSet(t,sizeof(*t),0);
}

static Tile *Lookup(App *a, TileKey k)
{
    for (UInt16 i=0;i<SLOTS;++i)
        if (a->tiles[i].base[0] && a->tiles[i].layer==a->view.layer &&
            SameTile(k,a->tiles[i].key)) return &a->tiles[i];
    return NULL;
}

static Tile *Vacant(App *a)
{
    Tile *t=&a->tiles[0];
    for (UInt16 i=0;i<SLOTS;++i) {
        if (!a->tiles[i].base[0]) return &a->tiles[i];
        if (a->tiles[i].used<t->used) t=&a->tiles[i];
    }
    return t;
}

static Boolean Decode(App *a, TileKey k, const UInt8 *data, UInt32 n,
    UInt8 fallback, const CacheMeta *meta)
{
    Tile next;
    MemSet(&next,sizeof(next),0);
    Tile *t=Lookup(a,k);
    if (!t) t=Vacant(a);
    if (t->base[0] && (t->layer!=a->view.layer || !SameTile(t->key,k))) {
        // Recycle a displaced tile's bitmap bands and density wrappers.
        next=*t;
        MemSet(t,sizeof(*t),0);
        if (next.labels) MemPtrFree(next.labels);
        next.labels=NULL;
    }
    // Some 5.x ROMs cannot allocate a single 128 KiB BmpCreate bitmap.
    UInt32 args[13]={nativePng,(UInt32)data,n,0,0,0,0,(UInt32)a->scratch,
        a->view.layer==night && fallback!=overviewFallback};
    if (fallback>=previewParent) {
        UInt16 shift=fallback-previewParent+1;
        UInt32 mask=(1UL<<shift)-1;
        args[9]=shift;
        args[10]=k.x&mask;
        args[11]=k.y&mask;
    }
    Boolean vector=a->view.layer==shortbread;
    if (vector) {
        UInt16 shift=k.z>SHORTBREAD_MAX_Z ? k.z-SHORTBREAD_MAX_Z : 0;
        UInt32 mask=(1UL<<shift)-1;
        args[0]=nativeVectorDraw;
        args[8]=shift;
        args[9]=k.x&mask;
        args[10]=k.y&mask;
        args[11]=a->vectorOptions;
        if (shift && !a->sceneTried) {
            a->sceneTried=true;
#ifdef MAP_VECTOR_BENCH
            UInt32 started=TimGetTicks();
#endif
            if (!a->sceneIndex) a->sceneIndex=MapAlloc(VECTOR_SCENE_CAPACITY);
            if (a->sceneIndex) {
                UInt32 scene[]={nativeIndexScene,(UInt32)data,n,(UInt32)a->sceneIndex};
                a->sceneReady=PceNativeCall(a->native,scene)!=0;
            }
#ifdef MAP_VECTOR_BENCH
            a->indexTicks+=TimGetTicks()-started;
#endif
        }
        args[12]=shift && a->sceneReady ? (UInt32)a->sceneIndex : 0;
    }
    for (UInt16 i=0;i<4;++i) {
        if (!next.base[i]) {
            UInt16 err=0,stride=0;
            Coord w,h;
            next.base[i]=BmpCreate(256,64,16,NULL,&err);
            if (!next.base[i] || err) goto failed;
            UInt8 *pixels=BmpGetBits(next.base[i]);
            BmpGetDimensions(next.base[i],&w,&h,&stride);
            if (!pixels || w!=256 || h!=64 || stride!=512) goto failed;
            if (a->highDensity) {
                next.dense[i]=(BitmapType *)BmpCreateBitmapV3(next.base[i],kDensityDouble,pixels,NULL);
                if (!next.dense[i]) goto failed;
            }
        }
        args[3+i]=(UInt32)BmpGetBits(next.base[i]);
    }
#ifdef MAP_VECTOR_BENCH
    UInt32 started=TimGetTicks();
#endif
    if (!PceNativeCall(a->native,args)) goto failed;
#ifdef MAP_VECTOR_BENCH
    UInt32 elapsed=TimGetTicks()-started;
    a->renderTicks+=elapsed;
    if (elapsed>a->peakTicks) a->peakTicks=elapsed;
    ++a->rendered;
#endif
    if (vector && k.z>=VECTOR_LABEL_MIN_Z) {
        next.labels=MemPtrNew(sizeof(*next.labels));
        if (next.labels) {
            if (args[8] && !a->indexTried) {
                a->indexTried=true;
#ifdef MAP_VECTOR_BENCH
                started=TimGetTicks();
#endif
                if (!a->labelIndex) a->labelIndex=MapAlloc(VECTOR_INDEX_CAPACITY);
                if (a->labelIndex) {
                    UInt32 index[]={nativeIndexLabels,(UInt32)data,n,(UInt32)a->scratch,
                        (UInt32)a->labelIndex};
                    a->indexReady=PceNativeCall(a->native,index)!=0;
                }
#ifdef MAP_VECTOR_BENCH
                a->indexTicks+=TimGetTicks()-started;
#endif
            }
            UInt32 labels[]={nativeVectorLabels,(UInt32)data,n,(UInt32)a->scratch,
                args[8],args[9],args[10],(UInt32)next.labels,
                a->indexReady ? (UInt32)a->labelIndex : 0};
#ifdef MAP_VECTOR_BENCH
            started=TimGetTicks();
#endif
            PceNativeCall(a->native,labels);
#ifdef MAP_VECTOR_BENCH
            a->extractTicks+=TimGetTicks()-started;
#endif
        }
    }
    Destroy(t);
    next.key=k; next.layer=a->view.layer; next.fallback=fallback; next.used=++a->useCount;
    next.meta=meta ? *meta : (CacheMeta){0};
    *t=next;
    return true;
failed:
    Destroy(&next);
    return false;
}

static Boolean LoadOverview(App *a, TileKey k)
{
    if (k.z>OVERVIEW_Z) return false;
    UInt16 end=(1U<<(2*k.z));
    UInt16 index=1000+(end-1)/3+(k.y<<k.z)+k.x;
    MemHandle h=DmGetResource('wrld',index);
    if (!h) return false;
    const UInt8 *p=MemHandleLock(h);
    Boolean ok=p && Decode(a,k,p,MemHandleSize(h),
        a->view.layer==overview ? exactTile : overviewFallback,NULL);
    if (p) MemHandleUnlock(h);
    DmReleaseResource(h);
    return ok;
}

static TileKey ParentKey(TileKey k)
{
    if (k.z>SHORTBREAD_MAX_Z) {
        UInt16 shift=k.z-SHORTBREAD_MAX_Z;
        k.x>>=shift;
        k.y>>=shift;
        k.z=SHORTBREAD_MAX_Z;
    }
    return k;
}

static Boolean UnpackVector(App *a, TileKey parent, const UInt8 *data, UInt32 size,
    const CacheMeta *meta)
{
    if (!a->vectorData) a->vectorData=MapAlloc(VECTOR_CAPACITY);
    if (!a->vectorData) return false;
    UInt32 args[]={nativeVectorUnpack,(UInt32)data,size,(UInt32)a->vectorData,
        VECTOR_CAPACITY,(UInt32)a->scratch};
#ifdef MAP_VECTOR_BENCH
    UInt32 started=TimGetTicks();
#endif
    a->vectorSize=PceNativeCall(a->native,args);
#ifdef MAP_VECTOR_BENCH
    a->unpackTicks=TimGetTicks()-started;
#endif
    a->indexReady=a->indexTried=a->sceneReady=a->sceneTried=false;
    a->vectorKey=parent;
    a->vectorMeta=*meta;
    return a->vectorSize!=0;
}

static Boolean LoadCached(App *a, TileKey k)
{
    Boolean vector=a->view.layer==shortbread;
    TileKey parent=vector ? ParentKey(k) : k;
    UInt32 bytes;
    CacheMeta meta;
    if (!CacheRead(&a->cache,parent,&a->net.body,&a->net.bodyCapacity,&bytes,&meta))
        return false;
    const UInt8 *data=a->net.body;
    Boolean ok;
    if (vector) {
        ok=UnpackVector(a,parent,data,bytes,&meta) &&
            Decode(a,k,a->vectorData,a->vectorSize,exactTile,&meta);
        if (!ok) a->vectorSize=0;
    } else {
        ok=Decode(a,k,data,bytes,exactTile,&meta);
    }
    return ok;
}

static Boolean LoadRasterPreview(App *a, TileKey key)
{
    if (!a->offline || a->view.layer==shortbread) return false;
    TileKey parent=key;
    for (UInt8 kind=previewParent; kind<=previewGrandparent && parent.z; ++kind) {
        --parent.z; parent.x>>=1; parent.y>>=1;
        CacheMeta meta;
        UInt32 bytes;
        if (CacheRead(&a->cache,parent,&a->net.body,&a->net.bodyCapacity,&bytes,&meta) &&
            Decode(a,key,a->net.body,bytes,kind,NULL)) return true;
    }
    return false;
}

static Int32 PixelUnits(const App *a) { return (WORLD>>a->view.z)/a->tileSide; }
static Int32 TileFloor(Int32 position, Coord side)
{ return position>=0 ? position/side : -((-position+side-1)/side); }

static UInt16 VisibleTiles(App *a, Visible *v)
{
    Int32 unit=PixelUnits(a), side=a->tileSide;
    Int32 left=(Int32)(a->view.x/(UInt32)unit)-a->map.extent.x/2;
    Int32 top=(Int32)(a->view.y/(UInt32)unit)-a->map.extent.y/2;
    Int32 n=1L<<a->view.z, x0=TileFloor(left,side), y0=TileFloor(top,side);
    UInt16 count=0;
    for (Int32 y=y0;y*side<top+a->map.extent.y;++y) {
        if (y<0 || y>=n) continue;
        for (Int32 x=x0;x*side<left+a->map.extent.x;++x) {
            if (count==SLOTS) return count;
            Visible *p=v+count++;
            p->key=(TileKey){a->sourceId,(UInt32)(x&(n-1)),(UInt32)y,a->view.z};
            p->x=(Coord)(x*side-left);
            p->y=(Coord)(a->map.topLeft.y+y*side-top);
        }
    }
    return count;
}

static void NearTiles(const App *a, const Visible *v, UInt16 count, UInt16 *order)
{
    UInt32 distance[SLOTS];
    for (UInt16 i=0;i<count;++i) {
        Int32 dx=v[i].x+a->tileSide/2-a->map.extent.x/2;
        Int32 dy=v[i].y+a->tileSide/2-TOP-a->map.extent.y/2;
        distance[i]=dx*dx+dy*dy;
        UInt16 pos=i;
        while (pos && distance[order[pos-1]]>distance[i]) {
            order[pos]=order[pos-1];
            --pos;
        }
        order[pos]=i;
    }
}

static IndexedColorType Color(UInt8 r, UInt8 g, UInt8 b)
{
    RGBColorType c={0,r,g,b};
    return WinRGBToIndex(&c);
}

static void Text(const Char *s, Coord x, Coord y, Coord width)
{
    if (width<=0) return;
    Int16 len=StrLen(s), w=width;
    Boolean fits;
    FntCharsInWidth(s,&w,&len,&fits);
    if (len>0) WinDrawChars(s,len,x,y);
}

static void Fill(Coord x, Coord y, Coord w, Coord h, IndexedColorType color)
{
    RectangleType rect;
    RctSetRectangle(&rect,x,y,w,h);
    WinSetForeColor(color);
    WinPaintRectangle(&rect,0);
}

static UInt8 GlyphIndex(UInt8 ch) { return ch>=32 ? ch-32 : 0; }

static void FreeLabelPainter(App *a)
{
    if (a->labelBitmap) BmpDelete(a->labelBitmap);
    if (a->labelBase) BmpDelete(a->labelBase);
    if (a->glyphs) MemPtrFree(a->glyphs);
    a->labelBitmap=a->labelBase=NULL;
    a->glyphs=NULL;
    a->labelPixels=NULL;
}

static void PrepareLabelPainter(App *a)
{
    if (a->labelPainterTried) return;
    a->labelPainterTried=true;
    UInt16 err=0, stride=0;
    BitmapType *glyph=BmpCreate(16,MAP_GLYPH_HEIGHT,1,NULL,&err);
    if (!glyph) return;
    WinHandle buffer=WinCreateBitmapWindow(glyph,&err);
    if (!buffer) {
        BmpDelete(glyph);
        return;
    }
    a->glyphs=MemPtrNew(MAP_GLYPH_COUNT*sizeof(*a->glyphs));
    if (a->glyphs) {
        WinPushDrawState();
        WinHandle oldWin=WinSetDrawWindow(buffer);
        UInt16 oldCoord=WinSetCoordinateSystem(kCoordinatesStandard);
        WinResetClip();
        WinSetBackColor(0);
        WinSetTextColor(1);
        FntSetFont(boldFont);
        const UInt8 *bits=BmpGetBits(glyph);
        BmpGetDimensions(glyph,NULL,NULL,&stride);
        for (UInt16 ch=32;ch<256;++ch) {
            MapGlyph *g=a->glyphs+GlyphIndex((UInt8)ch);
            Char text=(Char)ch;
            g->width=FntCharsWidth(&text,1);
            if (g->width>16) g->width=16;
            WinEraseWindow();
            if (ch>32) WinDrawChars(&text,1,0,0);
            for (UInt16 y=0;y<MAP_GLYPH_HEIGHT;++y) {
                g->rows[y][0]=bits[y*stride];
                g->rows[y][1]=bits[y*stride+1];
            }
        }
        WinSetCoordinateSystem(oldCoord);
        WinSetDrawWindow(oldWin);
        WinPopDrawState();
    }
    WinDeleteWindow(buffer,false);
    BmpDelete(glyph);
    if (!a->glyphs) return;

    a->labelBase=BmpCreate(MAP_LABEL_BITMAP_SIDE,MAP_LABEL_BITMAP_SIDE,8,NULL,&err);
    if (a->labelBase) {
        a->labelPixels=BmpGetBits(a->labelBase);
        BmpGetDimensions(a->labelBase,NULL,NULL,&stride);
        if (a->labelPixels && stride==MAP_LABEL_BITMAP_SIDE) {
            a->labelBitmap=(BitmapType *)BmpCreateBitmapV3(a->labelBase,
                a->highDensity ? kDensityDouble : kDensityLow,a->labelPixels,NULL);
        }
    }
    if (!a->labelBitmap) {
        FreeLabelPainter(a);
        return;
    }
    a->labelHalo=Color(255,253,241);
    a->labelStreet=Color(35,53,51);
    a->labelLandmark=Color(38,42,72);
    a->labelKey=255;
    while (a->labelKey==a->labelHalo || a->labelKey==a->labelStreet ||
        a->labelKey==a->labelLandmark) --a->labelKey;
    BmpSetTransparentValue(a->labelBitmap,a->labelKey);
}

static Coord LabelAdvance(const App *a, const Char *name)
{
    Coord advance=0;
    while (*name) advance+=a->glyphs[GlyphIndex((UInt8)*name++)].width;
    return advance;
}

static void PaintLabel(App *a, const Char *name, const RectangleType *box,
    Int16 ux, Int16 uy, UInt8 ink)
{
    UInt32 colors=(UInt32)a->labelKey<<16 | (UInt32)a->labelHalo<<8 | ink;
    UInt32 args[]={nativeLabelBitmap,(UInt32)a->glyphs,(UInt32)name,
        (UInt32)a->labelPixels,box->extent.x,box->extent.y,(UInt32)(Int32)ux,(UInt32)(Int32)uy,colors};
    PceNativeCall(a->native,args);
    // The reusable bitmap is clipped to the portion just painted by the ARMlet.
    WinSetClip(box);
    WinDrawBitmap(a->labelBitmap,box->topLeft.x,box->topLeft.y);
}

static UInt16 LabelScale(const App *a) { return a->highDensity ? 2 : 1; }

static void LabelDirection(Coord dx, Coord dy, Int16 *ux, Int16 *uy)
{
    *ux=256; *uy=0;
    if (!dy) return;
    if (dx<0 || (!dx && dy>0)) { dx=-dx; dy=-dy; }
    UInt32 square=((Int32)dx*dx+(Int32)dy*dy)<<8;
    UInt32 root=0;
    for (UInt32 bit=1UL<<24;bit;bit>>=2) {
        if (square>=root+bit) {
            square-=root+bit;
            root=(root>>1)+bit;
        } else root>>=1;
    }
    *ux=(Int16)((Int32)dx*4096/(Int32)root);
    *uy=(Int16)((Int32)dy*4096/(Int32)root);
}

typedef struct {
    Coord x,y;
    RectangleType caption;
} Marker;

static Boolean LocateMarker(App *a, Marker *m)
{
    if (!a->view.pin) return false;
    Int32 dx=(Int32)a->view.markX-(Int32)a->view.x;
    if (dx>WORLD/2) dx-=WORLD;
    if (dx<-WORLD/2) dx+=WORLD;
    Int32 unit=PixelUnits(a);
    Int32 x=a->map.extent.x/2+dx/unit;
    Int32 y=a->map.topLeft.y+a->map.extent.y/2+
        ((Int32)a->view.markY-(Int32)a->view.y)/unit;
    if (x<0 || x>=a->map.extent.x || y<a->map.topLeft.y ||
        y>=a->map.topLeft.y+a->map.extent.y) return false;
    m->x=(Coord)x; m->y=(Coord)y;
    Boolean small=a->view.layer==shortbread && a->labelBitmap;
    Coord scale=LabelScale(a);
    Coord captionHeight=small ? (MAP_GLYPH_HEIGHT+4+scale-1)/scale : 12;
    Coord captionTop=(Coord)y-12-captionHeight;
    if (a->view.name[0] && captionTop>=TOP) {
        Coord label=small ? (LabelAdvance(a,a->view.name)+4+scale-1)/scale :
            FntCharsWidth(a->view.name,StrLen(a->view.name))+8;
        Coord max=small && MAP_LABEL_BITMAP_SIDE/scale<a->width-6 ?
            MAP_LABEL_BITMAP_SIDE/scale : a->width-6;
        if (small && label>max) return true;
        if (label>max) label=max;
        Coord lx=(Coord)x-label/2;
        if (lx<3) lx=3;
        if (lx+label>a->width-3) lx=a->width-3-label;
        RctSetRectangle(&m->caption,lx,captionTop,label,captionHeight);
    }
    return true;
}

static void DrawMarker(App *a, const Marker *m)
{
    Fill(m->x-5,m->y-11,11,9,Color(255,252,228));
    Fill(m->x-4,m->y-10,9,7,Color(171,44,36));
    Fill(m->x-1,m->y-3,3,4,Color(171,44,36));
    if (!m->caption.extent.x) return;
    Coord lx=m->caption.topLeft.x, ly=m->caption.topLeft.y;
    if (a->view.layer==shortbread && a->labelBitmap) {
        UInt16 scale=LabelScale(a);
        RectangleType box;
        RctSetRectangle(&box,lx*scale,ly*scale,m->caption.extent.x*scale,m->caption.extent.y*scale);
        UInt16 mode=WinSetCoordinateSystem(a->highDensity ? kCoordinatesDouble : kCoordinatesStandard);
        PaintLabel(a,a->view.name,&box,256,0,a->labelLandmark);
        WinSetCoordinateSystem(mode);
        WinSetClip(&a->map);
    } else {
        IndexedColorType paper=Color(253,249,225);
        Fill(lx,ly,m->caption.extent.x,m->caption.extent.y,paper);
        WinSetBackColor(paper);
        WinSetTextColor(Color(46,42,34));
        Text(a->view.name,lx+4,ly,m->caption.extent.x-8);
    }
}

static void *Object(FormType *f, UInt16 id) { return FrmGetObjectPtr(f,FrmGetObjectIndex(f,id)); }

#define DRAWN_LABELS 16
#define DRAWN_LANDMARKS 4
typedef struct {
    RectangleType area;
    const Char *name;
} PlacedLabel;

static Boolean Overlap(const RectangleType *a, const RectangleType *b)
{
    return a->topLeft.x < b->topLeft.x+b->extent.x &&
           b->topLeft.x < a->topLeft.x+a->extent.x &&
           a->topLeft.y < b->topLeft.y+b->extent.y &&
           b->topLeft.y < a->topLeft.y+a->extent.y;
}

static Boolean LabelOccupied(const PlacedLabel *placed, UInt16 count, const RectangleType *bounds)
{
    for (UInt16 i=0;i<count;++i)
        if (Overlap(bounds,&placed[i].area)) return true;
    return false;
}

static Boolean PlaceLabel(PlacedLabel *placed, UInt16 count, const RectangleType *map,
    const Char *name, const RectangleType *box)
{
    Coord x=box->topLeft.x, y=box->topLeft.y, w=box->extent.x, h=box->extent.y;
    if (x<1 || x+w>=map->extent.x-1 || y<map->topLeft.y+1 ||
        y+h>=map->topLeft.y+map->extent.y-1) return false;
    RectangleType bounds;
    RctSetRectangle(&bounds,x-2,y-1,w+4,h+2);
    if (LabelOccupied(placed,count,&bounds)) return false;
    placed[count]=(PlacedLabel){bounds,name};
    return true;
}

static UInt16 DrawLabels(App *a, const Visible *v, Tile *const *tiles, UInt16 count,
    const Marker *marker)
{
    PlacedLabel placed[DRAWN_LABELS+DRAWN_LANDMARKS+3];
    UInt16 used=1, drawn=0, landmarkCount=0;
    UInt16 scale=LabelScale(a);
    Coord zoomHeight=2*ZOOM_HEIGHT+ZOOM_GAP;
    RectangleType map;
    RctSetRectangle(&map,0,TOP*scale,a->width*scale,a->map.extent.y*scale);
    Coord bottom=map.topLeft.y+map.extent.y;
    RctSetRectangle(&placed[0].area,(a->width-ZOOM_MARGIN-ZOOM_WIDTH)*scale,
        (a->height-FOOT-ZOOM_MARGIN-zoomHeight)*scale,ZOOM_WIDTH*scale,zoomHeight*scale);
    placed[0].name=NULL;
    if (marker) {
        RctSetRectangle(&placed[used].area,(marker->x-5)*scale,(marker->y-11)*scale,11*scale,12*scale);
        placed[used++].name=NULL;
        const RectangleType *r=&marker->caption;
        if (r->extent.x) {
            RctSetRectangle(&placed[used].area,r->topLeft.x*scale,r->topLeft.y*scale,
                r->extent.x*scale,r->extent.y*scale);
            placed[used++].name=NULL;
        }
    }
    IndexedColorType halo=Color(255,253,241), ink=Color(38,42,72);
    UInt16 oldCoord=WinSetCoordinateSystem(a->highDensity ? kCoordinatesDouble : kCoordinatesStandard);
    UInt16 order[SLOTS];
    NearTiles(a,v,count,order);
    for (UInt16 rank=5;rank>0;--rank) {
        Boolean landmark=rank>=4;
        if (!(a->vectorOptions & (landmark ? vectorLandmarks : vectorStreetNames))) continue;
        for (UInt16 pos=0;pos<count && drawn<DRAWN_LABELS;++pos) {
            if (landmark && landmarkCount>=DRAWN_LANDMARKS) break;
            UInt16 t=order[pos];
            const VectorLabels *labels=tiles[t] ? tiles[t]->labels : NULL;
            if (!labels) continue;
            const VectorLabel *items=landmark ? labels->landmarks : labels->streets;
            UInt16 n=landmark ? labels->landmarkCount : labels->streetCount;
            Coord originX=v[t].x*scale, originY=v[t].y*scale;
            for (UInt16 i=0;i<n && drawn<DRAWN_LABELS;++i) {
                if (landmark && landmarkCount>=DRAWN_LANDMARKS) break;
                const VectorLabel *item=items+i;
                if (item->priority!=rank) continue;
                Coord ax=originX+item->x, ay=originY+item->y;
                Coord bx=originX+item->endX, by=originY+item->endY;
                if ((ax<0 && bx<0) || (ax>=map.extent.x && bx>=map.extent.x) ||
                    (ay<map.topLeft.y && by<map.topLeft.y) || (ay>=bottom && by>=bottom)) continue;
                Boolean duplicate=false;
                for (UInt16 p=0;p<used;++p) {
                    if (placed[p].name && !StrCompare(placed[p].name,item->name)) {
                        duplicate=true;
                        break;
                    }
                }
                if (duplicate) continue;
                RectangleType dot;
                if (landmark) {
                    RctSetRectangle(&dot,ax-2,ay-2,5,5);
                    if (LabelOccupied(placed,used,&dot)) continue;
                }
                Int16 ux=256,uy=0;
                if (!landmark && (a->vectorOptions & vectorAngledNames))
                    LabelDirection(bx-ax,by-ay,&ux,&uy);
                Coord advance=LabelAdvance(a,item->name);
                Int16 sinAbs=uy<0 ? -uy : uy;
                Coord w=(((Int32)ux*advance+sinAbs*MAP_GLYPH_HEIGHT+255)>>8)+4;
                Coord h=(((Int32)sinAbs*advance+ux*MAP_GLYPH_HEIGHT+255)>>8)+4;
                if (w>MAP_LABEL_BITMAP_SIDE || h>MAP_LABEL_BITMAP_SIDE) continue;
                RectangleType box;
                Boolean fits=false;
                for (UInt16 pos=0;pos<5 && !fits;++pos) {
                    Coord x,y;
                    if (landmark) {
                        if (pos==4) break;
                        x=pos==0 ? ax+4 : pos==1 ? ax-w-4 : ax-w/2;
                        y=pos<2 ? ay-h/2 : pos==2 ? ay-h-2 : ay+2;
                    } else {
                        Int16 fraction=pos==0 ? 2 : pos==1 ? 1 : pos==2 ? 3 : pos==3 ? 0 : 4;
                        x=ax+(bx-ax)*fraction/4-w/2;
                        y=ay+(by-ay)*fraction/4-h/2;
                    }
                    RctSetRectangle(&box,x,y,w,h);
                    fits=PlaceLabel(placed,used,&map,item->name,&box);
                }
                if (!fits) continue;
                ++used; ++drawn;
                if (landmark) {
                    ++landmarkCount;
                    placed[used++]=(PlacedLabel){dot,NULL};
                    WinSetClip(&map);
                    Fill(dot.topLeft.x,dot.topLeft.y,dot.extent.x,dot.extent.y,halo);
                    Fill(ax-1,ay-1,3,3,ink);
                }
                PaintLabel(a,item->name,&box,ux,uy,landmark ? a->labelLandmark : a->labelStreet);
            }
        }
    }
    WinSetCoordinateSystem(oldCoord);
    WinSetClip(&a->map);
    return drawn;
}

static void DrawDownloaded(App *a, Boolean force)
{
    UInt32 value=a->net.downloaded/100;
    UInt32 now=TimGetTicks();
    if (!force && (value==a->lastCounter || now-a->counterAt<SysTicksPerSecond()/5))
        return;
    a->lastCounter=value;
    a->counterAt=now;

    WinPushDrawState();
    RectangleType box, clip;
    WinGetClip(&clip);
    RctSetRectangle(&box,a->width-76,1,76,12);
    WinSetClip(&box);
    IndexedColorType paper=UIColorGetTableEntryIndex(UIFormFill);
    Fill(box.topLeft.x,box.topLeft.y,box.extent.x,box.extent.y,paper);
    WinSetBackColor(paper);
    WinSetTextColor(UIColorGetTableEntryIndex(UIFieldText));
    FntSetFont(stdFont);
    Char text[24];
    StrPrintF(text,"%s%lu.%u kB",a->offline ? "Off " : "",value/10,(UInt16)(value%10));
    Coord x=a->width-4-FntCharsWidth(text,StrLen(text));
    WinDrawChars(text,StrLen(text),x,1);
    WinSetClip(&clip);
    WinPopDrawState();
}

static void DrawMap(App *a)
{
    if (!a->map.extent.y) return;
    if (a->view.layer==shortbread && (a->view.z>=VECTOR_LABEL_MIN_Z || a->view.pin))
        PrepareLabelPainter(a);
    RectangleType clip;
    WinGetClip(&clip);
    Boolean locked=WinScreenLock(winLockCopy)!=NULL;
    WinPushDrawState();
    WinSetClip(&a->map);
    Coord bottom=a->height-FOOT;
    IndexedColorType blank=a->view.layer==night ? Color(37,46,53) : Color(222,233,231);
    WinSetBackColor(blank);
    Visible v[SLOTS];
    Tile *tiles[SLOTS];
    UInt16 count=VisibleTiles(a,v), drawn=0, offline=0;
    Boolean preview=false;
    Boolean covered=count && v[0].y<=TOP && v[count-1].y+a->tileSide>=bottom;
    for (UInt16 i=0;i<count;++i) {
        tiles[i]=Lookup(a,v[i].key);
        if (!tiles[i]) covered=false;
    }
    if (!covered) Fill(0,TOP,a->width,a->map.extent.y,blank);
    for (UInt16 i=0;i<count;++i) {
        Tile *t=tiles[i];
        if (!t) {
            WinSetForeColor(a->view.layer==night ? Color(55,65,72) : Color(198,213,210));
            WinDrawLine(v[i].x,v[i].y,v[i].x+a->tileSide-1,v[i].y);
            WinDrawLine(v[i].x,v[i].y,v[i].x,v[i].y+a->tileSide-1);
            continue;
        }
        t->used=++a->useCount;
        Coord bandHeight=a->tileSide/4;
        for (UInt16 band=0;band<4;++band) {
            Coord y=v[i].y+band*bandHeight;
            if (y<bottom && y+bandHeight>TOP)
                WinDrawBitmap(t->dense[band] ? t->dense[band] : t->base[band],v[i].x,y);
        }
        ++drawn;
        if (t->fallback==overviewFallback) ++offline;
        if (t->fallback>=previewParent) preview=true;
    }
    if (!drawn) {
        WinSetTextColor(a->view.layer==night ? Color(225,234,234) : Color(45,79,79));
        FntSetFont(boldFont);
        const Char *status=a->offline && a->view.layer!=overview ? "No cached tiles here" :
            a->blocked ? "Map tiles unavailable" : "Loading map...";
        Coord x=(a->width-FntCharsWidth(status,StrLen(status)))/2;
        Text(status,x,TOP+(a->map.extent.y-FntLineHeight())/2,a->width-x-3);
    }
    if (preview) {
        const Char *title="Cached preview";
        FntSetFont(stdFont);
        Coord width=FntCharsWidth(title,StrLen(title));
        IndexedColorType paper=Color(243,247,239);
        Fill(3,TOP+3,width+6,FntLineHeight()+2,paper);
        WinSetBackColor(paper);
        WinSetTextColor(Color(31,60,57));
        Text(title,6,TOP+4,width);
    }
    // Composite the labels after the complete terrain bitmap pass.
    Marker marker={0};
    Boolean hasMarker=LocateMarker(a,&marker);
    if (a->labelBitmap && a->view.layer==shortbread && a->view.z>=VECTOR_LABEL_MIN_Z && drawn) {
#ifdef MAP_VECTOR_BENCH
        UInt32 started=TimGetTicks();
        a->labelCount=DrawLabels(a,v,tiles,count,hasMarker ? &marker : NULL);
        a->overlayTicks=TimGetTicks()-started;
#else
        DrawLabels(a,v,tiles,count,hasMarker ? &marker : NULL);
#endif
    }
    if (hasMarker) DrawMarker(a,&marker);
#ifdef MAP_VECTOR_BENCH
    if (a->rendered) {
        Boolean vector=a->view.layer==shortbread;
        Char stats[80];
        FntSetFont(stdFont);
        StrPrintF(stats,"n=%lu draw=%lu ms max=%lu",a->rendered,
            a->renderTicks*1000/SysTicksPerSecond()/a->rendered,
            a->peakTicks*1000/SysTicksPerSecond());
        Fill(2,TOP+2,FntCharsWidth(stats,StrLen(stats))+4,vector ? 35 : 13,Color(255,255,255));
        WinSetBackColor(Color(255,255,255));
        WinSetTextColor(Color(0,0,0));
        Text(stats,4,TOP+2,a->width-8);
        if (vector) {
            StrPrintF(stats,"inflate %lu index %lu ms",a->unpackTicks*1000/SysTicksPerSecond(),
                a->indexTicks*1000/SysTicksPerSecond());
            Text(stats,4,TOP+13,a->width-8);
            StrPrintF(stats,"labels %lu + %lu ms (%u)",
                a->extractTicks*1000/SysTicksPerSecond()/a->rendered,
                a->overlayTicks*1000/SysTicksPerSecond(),a->labelCount);
            Text(stats,4,TOP+24,a->width-8);
        }
    }
#endif
    WinSetClip(&clip);
    Fill(0,bottom,a->width,1,Color(96,126,121));
    IndexedColorType paper=Color(243,247,239);
    Fill(0,bottom+1,a->width,FOOT-1,paper);
    WinSetBackColor(paper);
    WinSetTextColor(Color(31,60,57));
    FntSetFont(stdFont);
    Char zoom[9];
    StrPrintF(zoom,"z%u",(UInt16)a->view.z);
    Coord zoomX=a->width-FntCharsWidth(zoom,StrLen(zoom))-4;
    Coord textY=bottom+1+(FOOT-1-FntLineHeight())/2;
    Text(zoom,zoomX,textY,a->width-zoomX-4);
    const Char *credit;
    if (a->view.layer==cycle)
        credit="CyclOSM / \251 OSM contributors";
    else if (a->view.layer==overview || (drawn && drawn==offline))
        credit="Made with Natural Earth";
    else if (offline)
        credit="\251 OpenStreetMap / Natural Earth";
    else
        credit=a->view.layer==custom ? "Custom tiles / OSM contributors" :
            "\251 OpenStreetMap contributors";
    Text(credit,4,textY,zoomX-8);
    WinPopDrawState();
    FormType *form=FrmGetActiveForm();
    CtlDrawControl(Object(form,PlusButton));
    CtlDrawControl(Object(form,MinusButton));
    DrawDownloaded(a,true);
    if (locked) WinScreenUnlock();
}

static UInt32 SourceID(const App *a)
{
    const Pref *p=&a->view;
    // Keep tiles fetched with the compatibility engine out of the normal OSM cache.
    UInt32 compatibility=0x80000000UL;
    if (p->layer==shortbread) return VECTOR_SOURCE;
    if (p->layer==cycle) return CYCLE_SOURCE;
    if (p->layer!=custom) return a->forceTls12 ? compatibility : 0;
    UInt32 hash=a->forceTls12 ? 2166136261UL^compatibility : 2166136261UL;
    for (const UInt8 *s=(const UInt8 *)p->source;*s;++s) hash=(hash^*s)*16777619UL;
    return hash && hash!=compatibility && hash!=VECTOR_SOURCE && hash!=CYCLE_SOURCE ? hash : 1;
}

static Boolean TileURL(const Char *source, TileKey k, Char *out, UInt16 cap)
{
    Char *p=out, *limit=out+cap-1;
    Boolean x=false,y=false,z=false;
    if (StrNCompare(source,"https://",8)) return false;
    while (*source && p<limit) {
        Char c=*source++;
        if (c=='{' && source[0] && source[1]=='}') {
            UInt32 value;
            if (*source=='z') { value=k.z; z=true; }
            else if (*source=='x') { value=k.x; x=true; }
            else if (*source=='y') { value=k.y; y=true; }
            else return false;
            source+=2;
            Char number[12];
            StrIToA(number,(Int32)value);
            UInt16 n=StrLen(number);
            if (n>limit-p) return false;
            MemMove(p,number,n);
            p+=n;
        } else {
            if (c<=32 || (UInt8)c>=127 || c=='{' || c=='}' || c=='#') return false;
            *p++=c;
        }
    }
    *p=0;
    return !*source && x && y && z;
}

static void Wake(App *a)
{
    MapNetStop(&a->net,false);
    a->needWork=true;
    a->settleAt=TimGetTicks()+SysTicksPerSecond()/7;
}

static void ViewportChanged(App *a)
{
    UInt32 now=TimGetTicks();
    if (a->net.phase!=netIdle && (Int32)(now-a->net.deadline)<0) {
        Boolean vector=a->view.layer==shortbread;
        TileKey source=vector ? ParentKey(a->loading) : a->loading;
        Visible visible[SLOTS];
        UInt16 order[SLOTS];
        UInt16 count=VisibleTiles(a,visible);
        NearTiles(a,visible,count,order);
        for (UInt16 i=0;i<count;++i) {
            TileKey key=visible[order[i]].key;
            if (!SameTile(source,vector ? ParentKey(key) : key)) continue;
            a->loading=key;
            a->needWork=true;
            a->settleAt=now;
            return;
        }
    }
    Wake(a);
}

static void Retry(App *a)
{
    a->blocked=false;
    a->retryAt=0;
    Wake(a);
}

static void NetworkFailure(App *a,UInt16 code)
{
    a->blocked=true;
    // Use a longer cooldown for provider throttling or explicit refusal.
    a->retryAt=TimGetTicks()+((code==429 || code==403 || code==200) ? 60UL : 12UL)*SysTicksPerSecond();
    a->needWork=false;
}

static Boolean FromSource(const Tile *t, TileKey key, Boolean vector)
{
    return t->base[0] && t->layer!=overview && vector==(t->layer==shortbread) &&
        SameTile(vector ? ParentKey(t->key) : t->key,key);
}

static Boolean CompleteTile(App *a, UInt16 step, Boolean vector)
{
    UInt32 now=TimGetSeconds();
    CacheMeta meta=MapNetMetadata(&a->net,now);
    UInt32 fresh=meta.expires>now+60 ? meta.expires : now+60;
    TileKey key=vector ? ParentKey(a->loading) : a->loading;
    if (step==requestNotModified) {
        for (UInt16 i=0;i<SLOTS;++i) {
            Tile *t=a->tiles+i;
            if (FromSource(t,key,vector)) t->meta.expires=fresh;
        }
        if (vector && a->vectorSize && SameTile(a->vectorKey,key))
            a->vectorMeta.expires=fresh;
        CacheTouch(&a->cache,key,meta.expires);
        return false;
    }
    const UInt8 *data=a->net.body;
    UInt32 bytes=a->net.length;
    CacheMeta renderMeta=meta;
    renderMeta.expires=fresh;
    if (vector) {
        if (!UnpackVector(a,key,data,bytes,&renderMeta) ||
            !Decode(a,a->loading,a->vectorData,a->vectorSize,exactTile,&renderMeta)) {
            a->vectorSize=0;
            NetworkFailure(a,0);
            return true;
        }
    } else if (!Decode(a,a->loading,data,bytes,exactTile,&renderMeta)) {
        NetworkFailure(a,0);
        return true;
    }
    Tile *updated=Lookup(a,a->loading);
    for (UInt16 i=0;i<SLOTS;++i) {
        Tile *t=a->tiles+i;
        if (t!=updated && FromSource(t,key,vector)) Destroy(t);
    }
    if (meta.expires) CacheWrite(&a->cache,key,data,bytes,&meta);
    else CacheTouch(&a->cache,key,0);
    return true;
}

static void PaintDrag(App *a, Boolean final)
{
    if (a->dragPending && (final || TimGetTicks()-a->dragFrame>=SysTicksPerSecond()/20)) {
        DrawMap(a);
        a->dragPending=false;
        a->dragFrame=TimGetTicks();
    }
}

static void Work(App *a)
{
    if (a->drag) {
        PaintDrag(a,false);
        return;
    }
    if (!a->needWork || (Int32)(TimGetTicks()-a->settleAt)<0) return;
    Boolean vector=a->view.layer==shortbread;
    if (a->net.phase!=netIdle) {
        UInt16 step=MapNetStep(&a->net);
        if (step==requestBusy) {
            DrawDownloaded(a,false);
        } else if (step==requestError) {
            NetworkFailure(a,a->net.code);
            DrawMap(a);
        } else if (CompleteTile(a,step,vector)) {
            DrawMap(a);
        } else {
            DrawDownloaded(a,false);
        }
        return;
    }

    Visible v[SLOTS];
    UInt16 order[SLOTS];
    UInt16 count=VisibleTiles(a,v), missing=count, expired=count;
    NearTiles(a,v,count,order);
    UInt32 now=TimGetSeconds();
    if (vector && a->vectorSize) {
        // Finish all visible children before loading a different parent.
        for (UInt16 i=0;i<count;++i) {
            TileKey key=v[order[i]].key;
            if (!SameTile(ParentKey(key),a->vectorKey) || Lookup(a,key)) continue;
            if (Decode(a,key,a->vectorData,a->vectorSize,exactTile,&a->vectorMeta)) {
                DrawMap(a);
                return;
            }
            a->vectorSize=0;
            NetworkFailure(a,0);
            DrawMap(a);
            return;
        }
    }
    for (UInt16 i=0;i<count;++i) {
        UInt16 pos=order[i];
        TileKey key=v[pos].key;
        Tile *t=Lookup(a,key);
        if (!t && (a->view.layer==overview ? LoadOverview(a,key) :
              LoadCached(a,key) || LoadRasterPreview(a,key) ||
              (!vector && a->view.layer!=cycle && LoadOverview(a,key)))) {
            DrawMap(a);
            return;
        }
        if (!t || t->fallback) {
            if (missing==count) missing=pos;
        } else if (expired==count && t->meta.expires && t->meta.expires<=now) {
            expired=pos;
        }
    }
    // Fill the visible gaps before refreshing any tile already on screen.
    UInt16 best=missing!=count ? missing : expired;
    Boolean wasBlocked=a->blocked;
    if (a->offline || a->view.layer==overview || best==count ||
        (a->blocked && (Int32)(TimGetTicks()-a->retryAt)<0)) {
        a->needWork=false;
    } else if (a->view.layer==custom && !a->view.source[0]) {
        NetworkFailure(a,0);
    } else {
        a->loading=v[best].key;
        TileKey key=vector ? ParentKey(a->loading) : a->loading;
        const Char *source=OSM;
        if (vector) source=SHORTBREAD;
        else if (a->view.layer==cycle) source=CYCLE;
        else if (a->view.layer==custom) source=a->view.source;
        Char url[224];
        if (!TileURL(source,key,url,sizeof(url))) NetworkFailure(a,0);
        else {
            Tile *cached=Lookup(a,a->loading);
            if (!MapNetStart(&a->net,url,cached && !cached->fallback ? &cached->meta : NULL,
                             a->forceTls12,vector))
                NetworkFailure(a,a->net.code);
            else a->blocked=false;
        }
    }
    if (a->blocked!=wasBlocked) DrawMap(a);
    else if (a->net.downloaded/100!=a->lastCounter) DrawDownloaded(a,!a->needWork);
}

static UInt8 MaxZoom(const App *a) { return a->view.layer==overview ? OVERVIEW_Z : MAX_Z; }

static void Zoom(App *a, Int16 amount)
{
    Int16 z=(Int16)a->view.z+amount;
    if (z<0 || z>MaxZoom(a)) return;
    a->view.z=(UInt8)z;
#ifdef MAP_VECTOR_BENCH
    a->unpackTicks=a->renderTicks=a->peakTicks=a->rendered=a->extractTicks=a->indexTicks=0;
#endif
    ViewportChanged(a);
    DrawMap(a);
}

static void Pan(App *a, Int16 dx, Int16 dy)
{
    Int32 scale=PixelUnits(a);
    a->view.x=(a->view.x+(Int32)dx*scale)&(WORLD-1);
    Int32 y=(Int32)a->view.y+(Int32)dy*scale;
    a->view.y=y<0 ? 0 : y>=WORLD ? WORLD-1 : y;
}

static void Layer(App *a, UInt8 layer)
{
    if (a->view.layer==layer) return;
    FreeLabelPainter(a);
    a->labelPainterTried=false;
    if (a->labelIndex) {
        MemPtrFree(a->labelIndex);
        a->labelIndex=NULL;
    }
    if (a->sceneIndex) {
        MemPtrFree(a->sceneIndex);
        a->sceneIndex=NULL;
    }
    a->indexReady=a->indexTried=a->sceneReady=a->sceneTried=false;
    if (a->vectorData) {
        MemPtrFree(a->vectorData);
        a->vectorData=NULL;
        a->vectorSize=0;
    }
#ifdef MAP_VECTOR_BENCH
    a->unpackTicks=a->renderTicks=a->peakTicks=a->rendered=a->extractTicks=a->indexTicks=0;
#endif
    a->view.layer=layer;
    if (a->view.z>MaxZoom(a)) a->view.z=MaxZoom(a);
    a->sourceId=SourceID(a);
    Retry(a);
}

static void Bounds(FormType *f,UInt16 id,Coord x,Coord y,Coord w,Coord h)
{
    RectangleType r;
    RctSetRectangle(&r,x,y,w,h);
    FrmSetObjectBounds(f,FrmGetObjectIndex(f,id),&r);
}

static void Layout(App *a, FormType *f, UInt16 id)
{
    Coord w,h;
    WinGetDisplayExtent(&w,&h);
    RectangleType r;
    RctSetRectangle(&r,0,0,w,h);
    WinSetBounds(FrmGetWindowHandle(f),&r);
    if (id==MainForm) {
        a->width=w; a->height=h;
        RctSetRectangle(&a->map,0,TOP,w,h-TOP-FOOT);
        Coord zoomX=w-ZOOM_MARGIN-ZOOM_WIDTH;
        Coord minusY=h-FOOT-ZOOM_MARGIN-ZOOM_HEIGHT;
        Bounds(f,PlusButton,zoomX,minusY-ZOOM_GAP-ZOOM_HEIGHT,ZOOM_WIDTH,ZOOM_HEIGHT);
        Bounds(f,MinusButton,zoomX,minusY,ZOOM_WIDTH,ZOOM_HEIGHT);
    } else if (id==SearchForm) {
        Bounds(f,SearchField,5,18,w-56,14);
        Bounds(f,FindButton,w-45,17,40,14);
        Bounds(f,ResultsList,5,57,w-10,77);
        Bounds(f,BackButton,w-49,140,44,14);
    } else if (id==SourceForm) {
        Bounds(f,SourceField,6,54,w-12,51);
        Bounds(f,CancelButton,w-56,139,50,15);
    } else if (id==ShortbreadForm) {
        Coord right=w/2;
        Bounds(f,ShowBuildings,right,34,w-right-4,13);
        Bounds(f,ShowLand,right,49,w-right-4,13);
        Bounds(f,ShowLandmarkNames,right,80,w-right-4,13);
        Bounds(f,ApplyShortbread,6,h-21,42,15);
        Bounds(f,ResetShortbread,(w-50)/2,h-21,50,15);
        Bounds(f,CancelShortbread,w-48,h-21,42,15);
    }
}

static Err ResizeNotice(SysNotifyParamType *p)
{
    EventType e;
    (void)p;
    MemSet(&e,sizeof(e),0); e.eType=winDisplayChangedEvent;
    EvtAddUniqueEventToQueue(&e,0,true);
    return errNone;
}

static void ConfigureDIA(App *a, FormType *f)
{
    UInt32 pin=0, flags=0;
    if (FtrGet(pinCreator,pinFtrAPIVersion,&pin)!=errNone || pin<pinAPIVersion1_0) return;
    if (pin>=pinAPIVersion1_1 &&
        FtrGet(sysFtrCreator,sysFtrNumInputAreaFlags,&flags)==errNone &&
        !(flags&grfFtrInputAreaFlagDynamic)) return;
    a->diaLegacy=pin<pinAPIVersion1_1;
    FrmSetDIAPolicyAttr(f,frmDIAPolicyCustom);
    WinSetConstraintsSize(FrmGetWindowHandle(f),160,160,1000,160,160,1000);
    PINSetInputTriggerState(pinInputTriggerEnabled);
    if (a->diaLegacy && !a->notify && SysCurAppDatabase(&a->card,&a->db)==errNone)
        a->notify=SysNotifyRegister(a->card,a->db,sysNotifyDisplayResizedEvent,
            ResizeNotice,sysNotifyNormalPriority,NULL)==errNone;
}

static Boolean ParseDegrees(const Char **pos,Int32 *value)
{
    const Char *p=*pos;
    while (*p==' ') ++p;
    Boolean negative=*p=='-';
    if (negative || *p=='+') ++p;
    if (*p<'0'||*p>'9') return false;
    Int32 integral=0, fraction=0, factor=100000;
    while (*p>='0' && *p<='9') {
        integral=integral*10+(*p++-'0');
        if (integral>180) return false;
    }
    if (*p=='.') {
        ++p;
        while (*p>='0' && *p<='9') { fraction+=(*p++-'0')*factor; factor/=10; }
    }
    while (*p==' ') ++p;
    *pos=p;
    *value=integral*1000000L+fraction;
    if (negative) *value=-*value;
    return true;
}

static Boolean Coordinates(App *a,const Char *s,Place *p)
{
    Int32 lat,lon;
    if (!ParseDegrees(&s,&lat) || *s++!=',' || !ParseDegrees(&s,&lon) || *s) return false;
    UInt32 args[3]={1,(UInt32)lat,(UInt32)lon};
    if (!PceNativeCall(a->native,args)) return false;
    p->x=args[1]; p->y=args[2]; StrCopy(p->label,"Coordinates");
    return true;
}

static UInt8 PlaceRank(const Char *name, UInt16 len, const Char *query, UInt16 n,
    UInt8 cutoff)
{
    if (len<n) return 0;
    if (!StrNCaselessCompare(name,query,n)) {
        // GeoNames records end in ", XX". Match with or without the country.
        return len==n || (len-n==4 && name[n]==',' && name[n+1]==' ') ? 3 : 2;
    }
    if (cutoff<=1 && len>n)
        for (UInt16 offset=1;offset<=len-n;++offset)
            if (!StrNCaselessCompare(name+offset,query,n)) return 1;
    return 0;
}

static void SearchText(App *a,FormType *f)
{
    if (!a->searchStatus) return;
    RectangleType rect;
    RctSetRectangle(&rect,5,35,a->width-10,16);
    WinEraseRectangle(&rect,0);
    Text(a->searchStatus,5,36,a->width-10);
    RctSetRectangle(&rect,5,46,a->width-10,11);
    WinEraseRectangle(&rect,0);
    Text("Places: GeoNames",5,46,a->width-10);
    (void)f;
}

static void ShowSearchResults(App *a, FormType *f)
{
    ListType *list=Object(f,ResultsList);
    UInt16 count=a->searching ? 0 : a->resultCount;
    for (UInt16 i=0;i<count;++i) a->choices[i]=a->results[i].label;
    LstSetListChoices(list,a->choices,count);
    LstSetSelection(list,count ? 0 : noListSelection);
    if (count) LstSetTopItem(list,0);
    LstDrawList(list);
    ControlType *use=Object(f,UseButton);
    CtlSetEnabled(use,count!=0);
    CtlDrawControl(use);
    SearchText(a,f);
}

static void EndSearch(App *a, FormType *f)
{
    a->searching=false;
    a->searchStatus=a->resultCount ? "Select a result below" :
        a->searchLength<2 ? "Enter a city or lat, lon" : "No places found";
    ShowSearchResults(a,f);
}

static void AddPlace(App *a, const UInt8 *p, const Char *name, UInt16 len)
{
    UInt8 *ranks=a->searchRanks;
    UInt8 rank=PlaceRank(name,len,a->query,a->searchLength,ranks[RESULTS-1]);
    if (rank<=ranks[RESULTS-1]) return;
    UInt16 pos=a->resultCount<RESULTS ? a->resultCount++ : RESULTS-1;
    // Preserve the population order for equal matches.
    while (pos && rank>ranks[pos-1]) {
        a->results[pos]=a->results[pos-1];
        ranks[pos]=ranks[pos-1];
        --pos;
    }
    ranks[pos]=rank;
    Place *result=a->results+pos;
    result->x=ReadBE(p);
    result->y=ReadBE(p+4);
    StrNCopy(result->label,name,sizeof(result->label)-1);
    result->label[sizeof(result->label)-1]=0;
}

static void SearchStep(App *a, FormType *f)
{
    UInt32 deadline=TimGetTicks()+1+SysTicksPerSecond()/50;
    UInt16 examined=0;
    while (a->searchRanks[RESULTS-1]!=3) {
        MemHandle h=DmGetResource('city',a->searchResource);
        if (!h) break;
        const UInt8 *base=MemHandleLock(h);
        Boolean pause=false;
        if (base) {
            UInt32 size=MemHandleSize(h);
            const UInt8 *end=base+size;
            const UInt8 *p=base+(a->searchOffset<=size ? a->searchOffset : size);
            while (end-p>8 && a->searchRanks[RESULTS-1]!=3) {
                const Char *name=(const Char *)p+8;
                UInt16 len=0;
                while (name+len<(const Char *)end && name[len]) ++len;
                if (name+len>=(const Char *)end) break;
                AddPlace(a,p,name,len);
                p+=9+len;
                ++examined;
                if (!(examined&31) && (examined>=512 || (Int32)(TimGetTicks()-deadline)>=0)) {
                    a->searchOffset=(UInt16)(p-base);
                    pause=true;
                    break;
                }
            }
            MemHandleUnlock(h);
        }
        DmReleaseResource(h);
        if (pause) return;
        ++a->searchResource;
        a->searchOffset=0;
    }
    EndSearch(a,f);
}

static void FindPlaces(App *a,FormType *f)
{
    FieldType *field=Object(f,SearchField);
    const Char *s=FldGetTextPtr(field);
    while (s && *s==' ') ++s;
    if (s) StrNCopy(a->query,s,sizeof(a->query)-1); else a->query[0]=0;
    a->query[sizeof(a->query)-1]=0;
    UInt16 n=StrLen(a->query);
    while (n && a->query[n-1]==' ') a->query[--n]=0;
    a->searchLength=n;
    a->resultCount=0;
    if (Coordinates(a,a->query,a->results)) {
        a->resultCount=1;
        EndSearch(a,f);
    } else if (n<2) {
        EndSearch(a,f);
    } else {
        a->searchResource=1000;
        a->searchOffset=0;
        MemSet(a->searchRanks,sizeof(a->searchRanks),0);
        a->searching=true;
        a->searchStatus="Searching...";
        ShowSearchResults(a,f);
    }
}

static void UsePlace(App *a,FormType *f)
{
    if (a->searching) return;
    Int16 row=LstGetSelection(Object(f,ResultsList));
    if (row<0 || (UInt16)row>=a->resultCount) return;
    Place *p=a->results+row;
    a->view.x=a->view.markX=p->x;
    a->view.y=a->view.markY=p->y;
    a->view.pin=true;
    StrNCopy(a->view.name,p->label,sizeof(a->view.name)-1);
    a->view.name[sizeof(a->view.name)-1]=0;
    if (a->view.layer==overview) a->view.z=OVERVIEW_Z;
    else if (!a->offline) a->view.z=a->blocked ? OVERVIEW_Z : 12;
    FrmGotoForm(MainForm);
}

static void SetField(FieldType *f,const Char *s)
{
    if (!s[0]) return;
    UInt16 length=StrLen(s)+1;
    MemHandle h=MemHandleNew(length);
    if (!h) return;
    Char *p=MemHandleLock(h);
    if (!p) { MemHandleFree(h); return; }
    MemMove(p,s,length);
    MemHandleUnlock(h);
    FldSetTextHandle(f,h);
}

static Boolean MainEvent(App *a,FormType *f,EventType *e)
{
    switch ((UInt16)e->eType) {
    case frmOpenEvent:
        ConfigureDIA(a,f);
        Layout(a,f,MainForm);
        FrmDrawForm(f);
        Wake(a);
        DrawMap(a);
        return true;
    case frmUpdateEvent:
        FrmDrawForm(f); DrawMap(a); return true;
    case winDisplayChangedEvent:
        Layout(a,f,MainForm);
        FrmDrawForm(f);
        ViewportChanged(a); DrawMap(a); return true;
    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case PlusButton: Zoom(a,1); return true;
        case MinusButton: Zoom(a,-1); return true;
        }
        break;
    case menuEvent:
        if (e->data.menu.itemID==MenuFind) {
            MapNetStop(&a->net,false);
            FrmGotoForm(SearchForm);
            return true;
        }
        if (e->data.menu.itemID==MenuShortbreadSettings) {
            MapNetStop(&a->net,false);
            FrmGotoForm(ShortbreadForm);
            return true;
        }
        if (e->data.menu.itemID>=MenuLayerStreets && e->data.menu.itemID<=MenuLayerCycle) {
            Layer(a,(UInt8)(e->data.menu.itemID-MenuLayerStreets));
            DrawMap(a);
            return true;
        }
        if (e->data.menu.itemID==MenuClearPin) {
            if (a->view.pin) {
                a->view.pin=false;
                a->view.name[0]=0;
                DrawMap(a);
            }
            return true;
        }
        if (e->data.menu.itemID==MenuOffline) {
            UInt16 choice=FrmCustomAlert(OfflineAlert,a->offline ? "Offline" : "Online",NULL,NULL);
            if (choice<2 && a->offline!=(choice==0)) {
                a->offline=choice==0;
                PrefSetAppPreferences(CREATOR,OFFLINE_PREF,VERSION,&a->offline,sizeof(a->offline),true);
                Retry(a);
                DrawMap(a);
            }
            return true;
        }
        if (e->data.menu.itemID==MenuRetry) {
            if (!a->offline) { Retry(a); DrawMap(a); }
            return true;
        }
        if (e->data.menu.itemID==MenuHttps) {
            UInt16 choice=FrmCustomAlert(HttpsAlert,a->forceTls12 ? "TLS 1.2" : "Modern",NULL,NULL);
            if (choice<2) {
                Boolean force=choice==1;
                if (a->forceTls12!=force) {
                    a->forceTls12=force;
                    PrefSetAppPreferences(CREATOR,HTTPS_PREF,VERSION,&force,sizeof(force),true);
                    a->sourceId=SourceID(a);
                }
                Retry(a);
                DrawMap(a);
            }
            return true;
        }
        if (e->data.menu.itemID==MenuAbout) { FrmAlert(HelpAlert); return true; }
        if (e->data.menu.itemID==MenuSource) {
            MapNetStop(&a->net,false); FrmGotoForm(SourceForm); return true;
        }
        break;
    case penDownEvent:
        if (e->screenY>=a->height-FOOT) {
            if (!a->offline) { Retry(a); DrawMap(a); }
            return true;
        }
        if (RctPtInRectangle(e->screenX,e->screenY,&a->map)) {
            RectangleType bounds;
            FrmGetObjectBounds(f,FrmGetObjectIndex(f,PlusButton),&bounds);
            if (RctPtInRectangle(e->screenX,e->screenY,&bounds)) return false;
            FrmGetObjectBounds(f,FrmGetObjectIndex(f,MinusButton),&bounds);
            if (RctPtInRectangle(e->screenX,e->screenY,&bounds)) return false;
            a->drag=true;
            a->dragMoved=a->dragPending=false;
            a->dragX=e->screenX; a->dragY=e->screenY;
            return true;
        }
        break;
    case penMoveEvent:
    case penUpEvent:
        if (a->drag) {
            Int16 dx=a->dragX-e->screenX,dy=a->dragY-e->screenY;
            Boolean released=e->eType==penUpEvent;
            UInt32 now=TimGetTicks();
            if (dx || dy) {
                if (!a->dragMoved) a->dragFrame=now-SysTicksPerSecond();
                a->dragMoved=a->dragPending=true;
                Pan(a,dx,dy);
                a->dragX=e->screenX; a->dragY=e->screenY;
            }
            PaintDrag(a,released);
            if (released) {
                a->drag=false;
                if (a->dragMoved) ViewportChanged(a);
            }
            return true;
        }
        break;
    case keyDownEvent: {
        WChar key=e->data.keyDown.chr;
        Int16 dx=0,dy=0;
        if (key==vchrPageUp || key==vchrRockerUp) dy=-38;
        else if (key==vchrPageDown || key==vchrRockerDown) dy=38;
        else if (key==vchrRockerLeft) dx=-38;
        else if (key==vchrRockerRight) dx=38;
        else if (key=='+' || key=='=') { Zoom(a,1); return true; }
        else if (key=='-') { Zoom(a,-1); return true; }
        else break;
        Pan(a,dx,dy); ViewportChanged(a); DrawMap(a); return true;
    }
    default: break;
    }
    return false;
}

static const struct {
    UInt16 control, option;
} shortbreadChoices[]={
    {ShowRoads,vectorRoads}, {ShowBuildings,vectorBuildings},
    {ShowWater,vectorWater}, {ShowLand,vectorLand},
    {ShowStreetNames,vectorStreetNames}, {ShowLandmarkNames,vectorLandmarks},
    {AngleStreetNames,vectorAngledNames}, {HighContrast,vectorHighContrast}
};

static void SetShortbreadOptions(FormType *f, UInt16 options)
{
    for (UInt16 i=0;i<sizeof(shortbreadChoices)/sizeof(*shortbreadChoices);++i)
        CtlSetValue(Object(f,shortbreadChoices[i].control),(options & shortbreadChoices[i].option)!=0);
}

static void SaveShortbreadOptions(App *a, FormType *f)
{
    UInt16 options=0;
    for (UInt16 i=0;i<sizeof(shortbreadChoices)/sizeof(*shortbreadChoices);++i)
        if (CtlGetValue(Object(f,shortbreadChoices[i].control)))
            options|=shortbreadChoices[i].option;
    UInt16 changed=a->vectorOptions^options;
    if (!changed) return;
    a->vectorOptions=options;
    PrefSetAppPreferences(CREATOR,SHORTBREAD_PREF,VERSION,&options,sizeof(options),true);
    if (changed & VECTOR_TILE_OPTIONS)
        for (UInt16 i=0;i<SLOTS;++i)
            if (a->tiles[i].layer==shortbread) Destroy(a->tiles+i);
}

static Boolean FormEvent(EventType *e)
{
    App *a=State();
    if (!a) return false;
    FormType *f=FrmGetActiveForm();
    UInt16 id=FrmGetFormId(f);
    if (id==MainForm) return MainEvent(a,f,e);
    switch ((UInt16)e->eType) {
    case frmOpenEvent:
        ConfigureDIA(a,f);
        Layout(a,f,id);
        if (id==SearchForm) {
            SetField(Object(f,SearchField),a->query);
            a->resultCount=0;
            a->searching=false;
            a->searchStatus="Enter a city or lat, lon";
            CtlSetEnabled(Object(f,UseButton),false);
        } else if (id==SourceForm) {
            SetField(Object(f,SourceField),a->view.source);
        } else if (id==ShortbreadForm) {
            SetShortbreadOptions(f,a->vectorOptions);
        }
        FrmDrawForm(f);
        if (id==SearchForm) {
            SearchText(a,f);
            FrmSetFocus(f,FrmGetObjectIndex(f,SearchField));
        } else if (id==SourceForm) FrmSetFocus(f,FrmGetObjectIndex(f,SourceField));
        return true;
    case frmUpdateEvent:
    case winDisplayChangedEvent:
        Layout(a,f,id); FrmDrawForm(f);
        if (id==SearchForm) SearchText(a,f);
        return true;
    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case BackButton:
            a->searching=false;
            FrmGotoForm(MainForm); return true;
        case CancelButton: case CancelShortbread:
            FrmGotoForm(MainForm); return true;
        case ApplyShortbread:
            SaveShortbreadOptions(a,f);
            FrmGotoForm(MainForm); return true;
        case ResetShortbread:
            SetShortbreadOptions(f,VECTOR_DEFAULT);
            FrmDrawForm(f); return true;
        case FindButton: FindPlaces(a,f); return true;
        case UseButton: UsePlace(a,f); return true;
        case SaveButton: {
            const Char *s=FldGetTextPtr(Object(f,SourceField));
            Char url[224];
            if (s && *s && !TileURL(s,(TileKey){0,1,2,3},url,sizeof(url))) {
                FrmAlert(SourceAlert); return true;
            }
            StrNCopy(a->view.source,s ? s : "",SOURCE_MAX);
            a->view.source[SOURCE_MAX]=0;
            if (a->view.layer!=custom && a->view.source[0]) Layer(a,custom);
            else if (a->view.layer==custom && !a->view.source[0]) Layer(a,streets);
            a->sourceId=SourceID(a);
            Retry(a);
            FrmGotoForm(MainForm);
            return true;
        }
        }
        break;
    case fldChangedEvent:
        if (id==SearchForm && a->searching) {
            const Char *text=FldGetTextPtr(Object(f,SearchField));
            if (!text || StrCompare(text,a->query)) {
                a->resultCount=0;
                a->searchLength=0;
                EndSearch(a,f);
            }
        }
        break;
    case keyDownEvent:
        if (id==SearchForm && (e->data.keyDown.chr=='\n' || e->data.keyDown.chr=='\r')) {
            FindPlaces(a,f); return true;
        }
        break;
    default: break;
    }
    return false;
}

static void LoadPrefs(App *a)
{
    UInt16 size=sizeof(a->view);
    Int16 version=PrefGetAppPreferences(CREATOR,0,&a->view,&size,true);
    if (version!=VERSION || size!=sizeof(a->view) || a->view.x>=WORLD ||
        a->view.y>=WORLD || a->view.z>MAX_Z) {
        MemSet(&a->view,sizeof(a->view),0);
        a->view.x=74484300UL; a->view.y=97906000UL;
        a->view.z=3;
    }
    a->view.name[sizeof(a->view.name)-1]=0;
    a->view.source[SOURCE_MAX]=0;
    a->view.layer=streets;
    UInt16 modeSize=sizeof(a->forceTls12);
    if (PrefGetAppPreferences(CREATOR,HTTPS_PREF,&a->forceTls12,&modeSize,true)!=VERSION ||
        modeSize!=sizeof(a->forceTls12) || a->forceTls12>true)
        a->forceTls12=true;
    UInt16 offlineSize=sizeof(a->offline);
    if (PrefGetAppPreferences(CREATOR,OFFLINE_PREF,&a->offline,&offlineSize,true)!=VERSION ||
        offlineSize!=sizeof(a->offline) || a->offline>true)
        a->offline=false;
    UInt16 optionsSize=sizeof(a->vectorOptions);
    if (PrefGetAppPreferences(CREATOR,SHORTBREAD_PREF,&a->vectorOptions,&optionsSize,true)!=VERSION ||
        optionsSize!=sizeof(a->vectorOptions) || (a->vectorOptions & ~VECTOR_OPTION_MASK))
        a->vectorOptions=VECTOR_DEFAULT;
    a->sourceId=SourceID(a);
}

UInt32 PilotMain(UInt16 command,void *parameters,UInt16 flags)
{
    (void)parameters; (void)flags;
    if (command!=sysAppLaunchCmdNormalLaunch) return errNone;
    UInt32 rom=0,cpu=0;
    if (FtrGet(sysFtrCreator,sysFtrNumROMVersion,&rom)!=errNone ||
        rom<sysMakeROMVersion(5,0,0,sysROMStageRelease,0) ||
        FtrGet(sysFtrCreator,sysFtrNumProcessorID,&cpu)!=errNone ||
        !sysFtrNumProcessorIsARM(cpu)) {
        FrmAlert(RomAlert); return sysErrRomIncompatible;
    }
    App *a=MemPtrNew(sizeof(*a));
    if (!a) return memErrNotEnoughSpace;
    MemSet(a,sizeof(*a),0);
    a->net.socket=-1;
    a->arm=DmGetResource('armc',ARM_RESOURCE);
    a->native=a->arm ? (NativeFuncType *)MemHandleLock(a->arm) : NULL;
    a->scratch=MemPtrNew(SCRATCH_SIZE);
    Err result=memErrNotEnoughSpace;
    if (!a->native || !a->scratch || FtrSet(CREATOR,0,(UInt32)a)!=errNone) {
        FrmAlert(MemoryAlert);
        goto done;
    }
    UInt32 density=0;
    a->highDensity=WinScreenGetAttribute(winScreenDensity,&density)==errNone &&
        density>=kDensityDouble;
    a->tileSide=a->highDensity ? 128 : 256;
    LoadPrefs(a);
    CacheOpen(&a->cache);
    FrmGotoForm(MainForm);
    EventType event;
    do {
        UInt16 active=FrmGetActiveFormID();
        Boolean pending=(a->needWork && !a->drag) || a->dragPending;
        Int32 wait=evtWaitForever;
        if (active==MainForm && pending) wait=2;
        if (active==SearchForm && a->searching) wait=1;
        EvtGetEvent(&event,wait);
        if (event.eType==appStopEvent) break;
        UInt16 menuError=0;
        if (!SysHandleEvent(&event) && !MenuHandleEvent(NULL,&event,&menuError)) {
            if (event.eType==frmLoadEvent) {
                UInt16 id=event.data.frmLoad.formID;
                FormType *f=FrmInitForm(id);
                if (!f) break;
                FrmSetActiveForm(f);
                FrmSetEventHandler(f,FormEvent);
            } else FrmDispatchEvent(&event);
        }
        if (event.eType==nilEvent) {
            FormType *form=FrmGetActiveForm();
            if (form && WinGetActiveWindow()==FrmGetWindowHandle(form)) {
                if (FrmGetFormId(form)==MainForm) Work(a);
                else if (FrmGetFormId(form)==SearchForm && a->searching) SearchStep(a,form);
            }
        }
    } while (true);
    FrmCloseAllForms();
    PrefSetAppPreferences(CREATOR,0,VERSION,&a->view,sizeof(a->view),true);
    if (a->notify) SysNotifyUnregister(a->card,a->db,sysNotifyDisplayResizedEvent,sysNotifyNormalPriority);
    MapNetClose(&a->net);
    CacheClose(&a->cache);
    for (UInt16 i=0;i<SLOTS;++i) Destroy(a->tiles+i);
    FtrUnregister(CREATOR,0);
    result=errNone;
done:
    if (a->native) MemHandleUnlock(a->arm);
    if (a->arm) DmReleaseResource(a->arm);
    if (a->scratch) MemPtrFree(a->scratch);
    if (a->vectorData) MemPtrFree(a->vectorData);
    if (a->labelIndex) MemPtrFree(a->labelIndex);
    if (a->sceneIndex) MemPtrFree(a->sceneIndex);
    FreeLabelPainter(a);
    MemPtrFree(a);
    return result;
}
