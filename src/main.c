// SPDX-License-Identifier: GPL-3.0-or-later
#include <PalmOS.h>
#include <PceNativeCall.h>
#include <PenInputMgr.h>
#include "resource.h"
#include "map.h"
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
#define SOURCE_MAX 150
#define OSM "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

enum { streets, night, overview, custom };
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
    Boolean fallback;
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
    UInt32 sourceId, useCount, retryAt, settleAt;
    MapNet net;
    TileCache cache;
    Tile tiles[SLOTS];
    TileKey loading;
    MemHandle arm;
    NativeFuncType *native;
    UInt8 *scratch;
    RectangleType map;
    Coord width,height,tileSide,dragX,dragY;
    Boolean highDensity,drag,needWork,blocked,diaLegacy,notify,forceTls12;
    UInt16 card;
    LocalID db;
    Place results[RESULTS];
    Char *choices[RESULTS];
    UInt16 resultCount;
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
    MemSet(t,sizeof(*t),0);
}

static Tile *Lookup(App *a, TileKey k)
{
    for (UInt16 i=0;i<SLOTS;++i)
        if (a->tiles[i].base[0] && SameTile(k,a->tiles[i].key)) return &a->tiles[i];
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

static Boolean Decode(App *a, TileKey k, const UInt8 *png, UInt32 n,
    Boolean fallback, const CacheMeta *meta)
{
    Tile next;
    MemSet(&next,sizeof(next),0);
    Tile *t=Lookup(a,k);
    if (!t) t=Vacant(a);
    if (t->base[0] && !SameTile(t->key,k)) Destroy(t);
    // Some 5.x ROMs cannot allocate a single 128 KiB BmpCreate bitmap.
    UInt32 args[9]={0,(UInt32)png,n,0,0,0,0,(UInt32)a->scratch,
        a->view.layer==night && !fallback};
    for (UInt16 i=0;i<4;++i) {
        UInt16 err=0,stride=0;
        Coord w,h;
        next.base[i]=BmpCreate(256,64,16,NULL,&err);
        if (!next.base[i] || err) goto failed;
        UInt8 *pixels=BmpGetBits(next.base[i]);
        BmpGetDimensions(next.base[i],&w,&h,&stride);
        if (!pixels || w!=256 || h!=64 || stride!=512) goto failed;
        args[3+i]=(UInt32)pixels;
        if (a->highDensity) {
            next.dense[i]=(BitmapType *)BmpCreateBitmapV3(next.base[i],kDensityDouble,pixels,NULL);
            if (!next.dense[i]) goto failed;
        }
    }
    if (!PceNativeCall(a->native,args)) goto failed;
    Destroy(t);
    next.key=k; next.fallback=fallback; next.used=++a->useCount;
    if (meta) next.meta=*meta;
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
    Boolean ok=p && Decode(a,k,p,MemHandleSize(h),a->view.layer!=overview,NULL);
    if (p) MemHandleUnlock(h);
    DmReleaseResource(h);
    return ok;
}

static Boolean LoadCached(App *a, TileKey k)
{
    UInt32 bytes;
    CacheMeta meta;
    UInt8 *data=CacheRead(&a->cache,k,&bytes,&meta);
    if (!data) return false;
    Boolean ok=Decode(a,k,data,bytes,false,&meta);
    MemPtrFree(data);
    return ok;
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

static void DrawMarker(App *a)
{
    if (!a->view.pin) return;
    Int32 dx=(Int32)a->view.markX-(Int32)a->view.x;
    if (dx>WORLD/2) dx-=WORLD;
    if (dx<-WORLD/2) dx+=WORLD;
    Int32 unit=PixelUnits(a);
    Int32 x=a->map.extent.x/2+dx/unit;
    Int32 y=a->map.topLeft.y+a->map.extent.y/2+
        ((Int32)a->view.markY-(Int32)a->view.y)/unit;
    if (x<0 || x>=a->map.extent.x || y<a->map.topLeft.y ||
        y>=a->map.topLeft.y+a->map.extent.y) return;
    Fill(x-5,y-11,11,9,Color(255,252,228));
    Fill(x-4,y-10,9,7,Color(171,44,36));
    Fill(x-1,y-3,3,4,Color(171,44,36));
    if (a->view.name[0] && y>a->map.topLeft.y+23) {
        Coord label=FntCharsWidth(a->view.name,StrLen(a->view.name))+8;
        if (label>a->width-6) label=a->width-6;
        Coord lx=(Coord)x-label/2;
        if (lx<3) lx=3;
        if (lx+label>a->width-3) lx=a->width-3-label;
        IndexedColorType paper=Color(253,249,225);
        Fill(lx,y-24,label,12,paper);
        WinSetBackColor(paper);
        WinSetTextColor(Color(46,42,34));
        Text(a->view.name,lx+4,y-24,label-8);
    }
}

static void *Object(FormType *f, UInt16 id) { return FrmGetObjectPtr(f,FrmGetObjectIndex(f,id)); }

static void DrawMap(App *a)
{
    if (!a->map.extent.y) return;
    RectangleType clip;
    WinGetClip(&clip);
    Boolean locked=WinScreenLock(winLockCopy)!=NULL;
    WinPushDrawState();
    WinSetClip(&a->map);
    UInt16 bottom=a->height-FOOT;
    IndexedColorType blank=a->view.layer==night ? Color(37,46,53) : Color(222,233,231);
    Fill(0,TOP,a->width,a->map.extent.y,blank);
    WinSetBackColor(blank);
    Visible v[SLOTS];
    UInt16 count=VisibleTiles(a,v), drawn=0, offline=0;
    for (UInt16 i=0;i<count;++i) {
        Tile *t=Lookup(a,v[i].key);
        if (!t) {
            WinSetForeColor(a->view.layer==night ? Color(55,65,72) : Color(198,213,210));
            WinDrawLine(v[i].x,v[i].y,v[i].x+a->tileSide-1,v[i].y);
            WinDrawLine(v[i].x,v[i].y,v[i].x,v[i].y+a->tileSide-1);
            continue;
        }
        t->used=++a->useCount;
        for (UInt16 band=0;band<4;++band)
            WinDrawBitmap(t->dense[band] ? t->dense[band] : t->base[band],
                v[i].x,v[i].y+band*(a->tileSide/4));
        ++drawn;
        if (t->fallback) ++offline;
    }
    if (!drawn) {
        WinSetTextColor(a->view.layer==night ? Color(225,234,234) : Color(45,79,79));
        FntSetFont(boldFont);
        const Char *status=a->blocked ? "Map tiles unavailable" : "Loading map...";
        Coord x=(a->width-FntCharsWidth(status,StrLen(status)))/2;
        Text(status,x,TOP+(a->map.extent.y-FntLineHeight())/2,a->width-x-3);
    }
    DrawMarker(a);
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
    if (a->view.layer==overview || (drawn && drawn==offline))
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
    if (locked) WinScreenUnlock();
}

static UInt32 SourceID(const App *a)
{
    const Pref *p=&a->view;
    // Keep tiles fetched with the compatibility engine out of the normal OSM cache.
    UInt32 compatibility=0x80000000UL;
    if (p->layer!=custom) return a->forceTls12 ? compatibility : 0;
    UInt32 hash=a->forceTls12 ? 2166136261UL^compatibility : 2166136261UL;
    for (const UInt8 *s=(const UInt8 *)p->source;*s;++s) hash=(hash^*s)*16777619UL;
    return hash && hash!=compatibility ? hash : 1;
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

static void Work(App *a)
{
    if (a->drag || !a->needWork || (Int32)(TimGetTicks()-a->settleAt)<0) return;
    if (a->net.phase!=netIdle) {
        UInt16 step=MapNetStep(&a->net);
        if (step==requestBusy) return;
        if (step==requestError) NetworkFailure(a,a->net.code);
        else {
            UInt32 now=TimGetSeconds();
            CacheMeta meta=MapNetMetadata(&a->net,now);
            if (step==requestNotModified) {
                Tile *t=Lookup(a,a->loading);
                if (t) t->meta.expires=meta.expires>now+60 ? meta.expires : now+60;
                CacheTouch(&a->cache,a->loading,meta.expires);
            } else if (Decode(a,a->loading,a->net.body,a->net.length,false,&meta)) {
                Tile *t=Lookup(a,a->loading);
                if (t->meta.expires<now+60) t->meta.expires=now+60;
                if (meta.expires) CacheWrite(&a->cache,a->loading,a->net.body,a->net.length,&meta);
                else CacheTouch(&a->cache,a->loading,0);
            } else NetworkFailure(a,0);
        }
        DrawMap(a);
        return;
    }

    Visible v[SLOTS];
    UInt16 count=VisibleTiles(a,v), best=count;
    Int32 distance=0x7fffffffL;
    UInt32 now=TimGetSeconds();
    for (UInt16 i=0;i<count;++i) {
        Tile *t=Lookup(a,v[i].key);
        if (!t && (a->view.layer==overview ? LoadOverview(a,v[i].key) :
              LoadCached(a,v[i].key) || LoadOverview(a,v[i].key))) {
            DrawMap(a);
            return;
        }
        if (t && !t->fallback && (t->meta.expires==0 || t->meta.expires>now)) continue;
        Int32 dx=v[i].x+a->tileSide/2-a->width/2, dy=v[i].y+a->tileSide/2-TOP-a->map.extent.y/2;
        Int32 d=dx*dx+dy*dy;
        if (d<distance) { best=i; distance=d; }
    }
    if (a->view.layer==overview || best==count ||
        (a->blocked && (Int32)(TimGetTicks()-a->retryAt)<0)) {
        a->needWork=false;
    } else if (a->view.layer==custom && !a->view.source[0]) {
        NetworkFailure(a,0);
    } else {
        a->loading=v[best].key;
        Char url[224];
        if (!TileURL(a->view.layer==custom ? a->view.source : OSM,a->loading,url,sizeof(url)))
            NetworkFailure(a,0);
        else {
            Tile *cached=Lookup(a,a->loading);
            if (!MapNetStart(&a->net,url,cached && !cached->fallback ? &cached->meta : NULL,
                             a->forceTls12))
                NetworkFailure(a,a->net.code);
            else a->blocked=false;
        }
    }
    DrawMap(a);
}

static UInt8 MaxZoom(const App *a) { return a->view.layer==overview ? OVERVIEW_Z : MAX_Z; }

static void Zoom(App *a, Int16 amount)
{
    Int16 z=(Int16)a->view.z+amount;
    if (z<0 || z>MaxZoom(a)) return;
    a->view.z=(UInt8)z;
    Wake(a);
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
    a->view.layer=layer;
    if (a->view.z>MaxZoom(a)) a->view.z=MaxZoom(a);
    a->sourceId=SourceID(a);
    for (UInt16 i=0;i<SLOTS;++i) Destroy(a->tiles+i);
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
    } else {
        Bounds(f,SourceField,6,54,w-12,51);
        Bounds(f,CancelButton,w-56,139,50,15);
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

static Boolean Contains(const Char *name,const Char *query,UInt16 n)
{
    for (UInt16 left=StrLen(name);left>=n;--left,++name)
        if (!StrNCaselessCompare(name,query,n)) return true;
    return false;
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

static void FindPlaces(App *a,FormType *f)
{
    FieldType *field=Object(f,SearchField);
    ListType *list=Object(f,ResultsList);
    const Char *s=FldGetTextPtr(field);
    while (s && *s==' ') ++s;
    if (s) StrNCopy(a->query,s,sizeof(a->query)-1); else a->query[0]=0;
    a->query[sizeof(a->query)-1]=0;
    UInt16 n=StrLen(a->query);
    while (n && a->query[n-1]==' ') a->query[--n]=0;
    a->resultCount=0;
    if (Coordinates(a,a->query,a->results)) a->resultCount=1;
    else if (n>=2) {
        // Prefer names beginning with the query, ordered by population.
        for (UInt16 pass=0;pass<2 && a->resultCount<RESULTS;++pass)
            for (UInt16 index=1000;a->resultCount<RESULTS;++index) {
                MemHandle h=DmGetResource('city',index);
                if (!h) break;
                const UInt8 *p=MemHandleLock(h);
                if (p) {
                    const UInt8 *end=p+MemHandleSize(h);
                    while (end-p>8 && a->resultCount<RESULTS) {
                        const Char *name=(const Char *)p+8;
                        UInt16 len=0;
                        while (name+len<(const Char *)end && name[len]) ++len;
                        if (name+len>=(const Char *)end) break;
                        Boolean prefix=len>=n && !StrNCaselessCompare(name,a->query,n);
                        if ((pass==0 && prefix) || (pass==1 && !prefix && Contains(name,a->query,n))) {
                            Place *result=a->results+a->resultCount++;
                            result->x=ReadBE(p);
                            result->y=ReadBE(p+4);
                            StrNCopy(result->label,name,sizeof(result->label)-1);
                            result->label[sizeof(result->label)-1]=0;
                        }
                        p+=9+len;
                    }
                    MemHandleUnlock(h);
                }
                DmReleaseResource(h);
            }
    }
    for (UInt16 i=0;i<a->resultCount;++i) a->choices[i]=a->results[i].label;
    LstSetListChoices(list,a->choices,a->resultCount);
    LstSetSelection(list,a->resultCount ? 0 : noListSelection);
    if (a->resultCount) LstSetTopItem(list,0);
    LstDrawList(list);
    a->searchStatus=a->resultCount ? "Select a result below" :
        n<2 ? "Enter a city or lat, lon" : "No places found";
    SearchText(a,f);
}

static void UsePlace(App *a,FormType *f)
{
    Int16 row=LstGetSelection(Object(f,ResultsList));
    if (row<0 || (UInt16)row>=a->resultCount) return;
    Place *p=a->results+row;
    a->view.x=a->view.markX=p->x;
    a->view.y=a->view.markY=p->y;
    a->view.pin=true;
    StrNCopy(a->view.name,p->label,sizeof(a->view.name)-1);
    a->view.name[sizeof(a->view.name)-1]=0;
    a->view.z=a->view.layer==overview ? OVERVIEW_Z :
        (a->blocked ? OVERVIEW_Z : 12);
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
        Wake(a); DrawMap(a); return true;
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
        if (e->data.menu.itemID>=MenuLayerStreets && e->data.menu.itemID<=MenuLayerCustom) {
            Layer(a,(UInt8)(e->data.menu.itemID-MenuLayerStreets));
            DrawMap(a);
            return true;
        }
        if (e->data.menu.itemID==MenuRetry) { Retry(a); DrawMap(a); return true; }
        if (e->data.menu.itemID==MenuHttps) {
            UInt16 choice=FrmCustomAlert(HttpsAlert,a->forceTls12 ? "TLS 1.2" : "Modern",NULL,NULL);
            if (choice<2) {
                Boolean force=choice==1;
                if (a->forceTls12!=force) {
                    a->forceTls12=force;
                    PrefSetAppPreferences(CREATOR,HTTPS_PREF,VERSION,&force,sizeof(force),true);
                    a->sourceId=SourceID(a);
                    for (UInt16 i=0;i<SLOTS;++i) Destroy(a->tiles+i);
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
        if (e->screenY>=a->height-FOOT) { Retry(a); DrawMap(a); return true; }
        if (RctPtInRectangle(e->screenX,e->screenY,&a->map)) {
            RectangleType bounds;
            FrmGetObjectBounds(f,FrmGetObjectIndex(f,PlusButton),&bounds);
            if (RctPtInRectangle(e->screenX,e->screenY,&bounds)) return false;
            FrmGetObjectBounds(f,FrmGetObjectIndex(f,MinusButton),&bounds);
            if (RctPtInRectangle(e->screenX,e->screenY,&bounds)) return false;
            MapNetStop(&a->net,false);
            a->drag=true;
            a->dragX=e->screenX; a->dragY=e->screenY;
            return true;
        }
        break;
    case penMoveEvent:
    case penUpEvent:
        if (a->drag) {
            Int16 dx=a->dragX-e->screenX,dy=a->dragY-e->screenY;
            if (dx || dy) {
                Pan(a,dx,dy);
                a->dragX=e->screenX; a->dragY=e->screenY;
                DrawMap(a);
            }
            if (e->eType==penUpEvent) { a->drag=false; Wake(a); }
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
        Pan(a,dx,dy); Wake(a); DrawMap(a); return true;
    }
    default: break;
    }
    return false;
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
            a->searchStatus="Enter a city or lat, lon";
        } else SetField(Object(f,SourceField),a->view.source);
        FrmDrawForm(f);
        if (id==SearchForm) {
            SearchText(a,f);
            FrmSetFocus(f,FrmGetObjectIndex(f,SearchField));
        } else FrmSetFocus(f,FrmGetObjectIndex(f,SourceField));
        return true;
    case frmUpdateEvent:
    case winDisplayChangedEvent:
        Layout(a,f,id); FrmDrawForm(f);
        if (id==SearchForm) SearchText(a,f);
        return true;
    case ctlSelectEvent:
        switch (e->data.ctlSelect.controlID) {
        case BackButton: case CancelButton: FrmGotoForm(MainForm); return true;
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
        EvtGetEvent(&event,a->needWork && FrmGetActiveFormID()==MainForm ? 2 : evtWaitForever);
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
        if (event.eType==nilEvent && FrmGetActiveFormID()==MainForm &&
            WinGetActiveWindow()==FrmGetWindowHandle(FrmGetActiveForm())) Work(a);
    } while (true);
    FrmCloseAllForms();
    PrefSetAppPreferences(CREATOR,0,VERSION,&a->view,sizeof(a->view),true);
    if (a->notify) SysNotifyUnregister(a->card,a->db,sysNotifyDisplayResizedEvent,sysNotifyNormalPriority);
    MapNetClose(&a->net);
    if (a->cache.db) DmCloseDatabase(a->cache.db);
    for (UInt16 i=0;i<SLOTS;++i) Destroy(a->tiles+i);
    FtrUnregister(CREATOR,0);
    result=errNone;
done:
    if (a->native) MemHandleUnlock(a->arm);
    if (a->arm) DmReleaseResource(a->arm);
    if (a->scratch) MemPtrFree(a->scratch);
    MemPtrFree(a);
    return result;
}
