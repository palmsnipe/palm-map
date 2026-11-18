// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"
#include "memory.h"
#include "palm_tls.h"

static UInt16 Failed(MapNet *n, const Char *message, UInt16 error)
{
    n->error = message;
    n->code = error;
    MapNetStop(n,false);
    return requestError;
}

void MapNetStop(MapNet *n, Boolean completed)
{
    Err e;
    if (n->session) {
        PalmTlsSessionIoResult result;
        MemSet(&result,sizeof(result),0);
        result.structSize=sizeof(result);
        if (completed) PalmTlsLibSessionClose(n->tls,n->session,&result);
        else PalmTlsLibSessionCancel(n->tls,n->session,&result);
        n->session=0;
    }
    if (n->phase != netIdle && n->socket >= 0)
        NetLibSocketClose(n->net,n->socket,1,&e);
    n->socket=-1;
    n->phase=netIdle;
}

void MapNetClose(MapNet *n)
{
    MapNetStop(n,false);
    if (n->netOpen) NetLibClose(n->net,false);
    if (n->netLoaded) SysLibRemove(n->net);
    if (n->tlsOpen) PalmTlsLibClose(n->tls);
    if (n->tlsLoaded) SysLibRemove(n->tls);
    if (n->trust) MemHandleUnlock(n->roots);
    if (n->roots) DmReleaseResource(n->roots);
    if (n->body) MemPtrFree(n->body);
}

// The Browser-only profile advertises TLS 1.2 as a negotiated version, but
// only the standalone engine can service the fixed TLS 1.2 selector.
static Boolean HasStandaloneTls12(void)
{
    LocalID id=DmFindDatabase(0,PALM_TLS_LIB_NAME);
    DmOpenRef db=id ? DmOpenDatabase(0,id,dmModeReadOnly) : NULL;
    if (!db) return false;
    Boolean found=DmFindResource(db,'TlsC',1,NULL)!=dmMaxRecordIndex &&
        DmFindResource(db,'TlsR',1,NULL)!=dmMaxRecordIndex;
    DmCloseDatabase(db);
    return found;
}

static Boolean StartLibraries(MapNet *n, Boolean forceTls12)
{
    Err e;
    UInt32 caps;
    const Char *missing=forceTls12 ? "Install PalmTLS TLS 1.2" : "Install PalmTLS HTTPS";
    if (!n->tlsOpen) {
        e=SysLibFind(PALM_TLS_LIB_NAME,&n->tls);
        if (e && !n->tlsLoaded) {
            e=SysLibLoad(sysFileTLibrary,PALM_TLS_LIB_CREATOR,&n->tls);
            n->tlsLoaded=!e;
        }
        if (e || (e=PalmTlsLibOpen(n->tls))!=errNone) {
            Failed(n,missing,e); return false;
        }
        n->tlsOpen=true;
    }
    UInt32 needed=PALM_TLS_CAP_NATIVE_ARM|PALM_TLS_CAP_COOPERATIVE_IO|PALM_TLS_CAP_CA_STORE|
        (forceTls12 ? PALM_TLS_CAP_TLS_1_2 : PALM_TLS_CAP_MODERN);
    if (PalmTlsLibGetCapabilities(n->tls,&caps)!=errNone || (caps & needed)!=needed ||
        (forceTls12 && !HasStandaloneTls12())) {
        Failed(n,missing,0); return false;
    }
    n->protocol=forceTls12 ? palmTlsProtocolTls12 : palmTlsProtocolModern;
    n->options=PALM_TLS_SESSION_ALLOW_RESUME;
    if (!forceTls12 && !(caps & PALM_TLS_CAP_HOST_RANDOM)) {
        if (caps & PALM_TLS_CAP_BROWSER_STYLUS_RANDOM)
            n->options=PALM_TLS_SESSION_REQUIRE_STYLUS_RANDOM;
        else {
            Failed(n,"PalmTLS needs entropy",0); return false;
        }
    }
    if (!n->roots) n->roots=DmGetResource('cert',1000);
    if (n->roots && !n->trust) {
        n->trust=MemHandleLock(n->roots);
        n->trustSize=MemHandleSize(n->roots);
    }
    if (!n->trust) { Failed(n,"Missing CA certificates",0); return false; }
    if (!n->body) n->body=MapAlloc(MAP_BODY_MAX);
    if (!n->body) { Failed(n,"Not enough memory",0); return false; }
    if (n->netOpen) return true;
    UInt16 interfaces=0;
    e=SysLibFind("Net.lib",&n->net);
    if (e && !n->netLoaded) {
        e=SysLibLoad(netLibType,netCreator,&n->net);
        n->netLoaded=!e;
    }
    if (!e) e=NetLibOpen(n->net,&interfaces);
    if (e && e!=netErrAlreadyOpen) { Failed(n,"Network unavailable",e); return false; }
    n->netOpen=true;
    return true;
}

static Int16 Collect(void *context, const UInt8 *data, UInt16 size)
{
    MapNet *n=context;
    if (n->parser.statusCode!=200 || size>MAP_BODY_MAX-n->length) return 1;
    MemMove(n->body+n->length,data,size);
    n->length+=size;
    return 0;
}

static void CollectHeader(void *context, const Char *name, UInt16 nameLength,
    const Char *value, UInt16 valueLength, Boolean overflow);

static Boolean ValidValidator(const Char *s)
{
    if (!s[0]) return false;
    for (;*s;++s) if ((UInt8)*s<32 || (UInt8)*s>126) return false;
    return true;
}

Boolean MapNetStart(MapNet *n, const Char *url, const CacheMeta *cached, Boolean forceTls12)
{
    MapNetStop(n,false);
    n->error=NULL;
    n->code=0;
    if (!HttpParseUrl(url,true,&n->url) || !n->url.secure) {
        Failed(n,"Invalid HTTPS tile URL",0); return false;
    }
    if (!StartLibraries(n,forceTls12)) return false;
    n->requestLength=HttpBuildGetRequestEx(&n->url,0,NULL,false,
        "PalmMap/0.1.5 (+https://github.com/palmsnipe)",false,
        n->request,sizeof(n->request));
    if (!n->requestLength) { Failed(n,"Tile URL is too long",0); return false; }
    n->conditional=false;
    if (cached && ValidValidator(cached->validator)) {
        const Char *name=cached->dateValidator ? "If-Modified-Since: " : "If-None-Match: ";
        if (n->requestLength>=2 &&
            n->requestLength+StrLen(name)+StrLen(cached->validator)+4<sizeof(n->request)) {
            Char *p=n->request+n->requestLength-2;
            p=StrCopy(p,name)+StrLen(name);
            p=StrCopy(p,cached->validator)+StrLen(cached->validator);
            StrCopy(p,"\r\n\r\n");
            n->requestLength=StrLen(n->request);
            n->conditional=true;
        }
    }
    n->written=0;
    n->length=0;
    MemSet(&n->response,sizeof(n->response),0);
    HttpParserInit(&n->parser,0,Collect,n);
    n->parser.headerProcP=CollectHeader;
    n->deadline=TimGetTicks() + 24UL*SysTicksPerSecond();
    n->phase=netDns;
    return true;
}

static UInt16 OpenSession(MapNet *n)
{
    PalmTlsSessionOpenParams p;
    PalmTlsSessionOpenResult r;
    MemSet(&p,sizeof(p),0);
    MemSet(&r,sizeof(r),0);
    p.structSize=sizeof(p);
    p.netRefNum=n->net;
    p.socket=n->socket;
    p.hostnameP=n->url.host;
    p.protocol=n->protocol;
    p.verifyMode=palmTlsVerifyCaStore;
    p.trustedPeerP=n->trust;
    p.trustedPeerLength=n->trustSize;
    p.timeoutTicks=1;
    p.options=n->options|PALM_TLS_SESSION_COOPERATIVE;
    r.structSize=sizeof(r);
    Err e=PalmTlsLibSessionOpen(n->tls,&p,&r);
    n->session=r.sessionId;
    if (!e && r.status==palmTlsStatusOk) n->phase=netWrite;
    else if (!e && r.status==palmTlsStatusWouldBlock && n->session) n->phase=netHandshake;
    else return Failed(n,r.status==palmTlsStatusRandomUnavailable ?
        "PalmTLS needs entropy" : "TLS connection failed",r.tlsError ? (UInt16)-r.tlsError : (e ? e : r.status));
    return requestBusy;
}

static UInt16 Dns(MapNet *n)
{
    Err e=0;
    NetHostInfoPtr host=NetLibGetHostByName(n->net,n->url.host,&n->dns,
        SysTicksPerSecond()/2,&e);
    if (!host || !host->addrListP || !host->addrListP[0]) {
        if (e==netErrTimeout || e==netErrWouldBlock) return requestBusy;
        return Failed(n,"Tile server DNS failed",e);
    }
    MemSet(&n->address,sizeof(n->address),0);
    n->address.family=netSocketAddrINET;
    n->address.port=NetHToNS(n->url.port);
    MemMove(&n->address.addr,host->addrListP[0],sizeof(n->address.addr));
    n->socket=NetLibSocketOpen(n->net,netSocketAddrINET,netSocketTypeStream,0,1,&e);
    if (n->socket < 0 || n->socket >= netFDSetSize) return Failed(n,"Cannot open socket",e);
    Int16 on=1;
    if (NetLibSocketOptionSet(n->net,n->socket,netSocketOptLevelSocket,
            netSocketOptSockNonBlocking,&on,sizeof(on),1,&e)<0)
        return Failed(n,"Cannot configure socket",e);
    n->phase=netConnect;
    return requestBusy;
}

static UInt16 Connect(MapNet *n)
{
    Err e=0;
    if (n->phase==netConnect) {
        if (NetLibSocketConnect(n->net,n->socket,(NetSocketAddrType *)&n->address,
                sizeof(n->address),1,&e) < 0) {
            if (e!=netErrWouldBlock && e!=netErrAlreadyInProgress)
                return Failed(n,"Tile server unreachable",e);
            n->phase=netConnecting;
            return requestBusy;
        }
    } else {
        NetFDSetType wr=0, ex=0;
        netFDSet(n->socket,&wr);
        netFDSet(n->socket,&ex);
        Int16 selected=NetLibSelect(n->net,n->socket+1,NULL,&wr,&ex,1,&e);
        if (selected<0) return Failed(n,"Tile server unreachable",e);
        if (!selected) return requestBusy;
        UInt16 len=sizeof(e);
        Err socketError=0;
        if (NetLibSocketOptionGet(n->net,n->socket,netSocketOptLevelSocket,
                netSocketOptSockErrorStatus,&socketError,&len,1,&e)<0 || socketError)
            return Failed(n,"Tile server unreachable",socketError ? socketError : e);
    }
    Int16 off=0;
    if (NetLibSocketOptionSet(n->net,n->socket,netSocketOptLevelSocket,
            netSocketOptSockNonBlocking,&off,sizeof(off),1,&e)<0)
        return Failed(n,"Cannot configure socket",e);
    n->phase=netTls;
    return requestBusy;
}

UInt16 MapNetStep(MapNet *n)
{
    if ((Int32)(TimGetTicks()-n->deadline)>=0) return Failed(n,"Tile request timed out",0);
    if (n->phase==netDns) return Dns(n);
    if (n->phase==netConnect || n->phase==netConnecting) return Connect(n);
    if (n->phase==netTls) return OpenSession(n);
    PalmTlsSessionIoParams p;
    PalmTlsSessionIoResult r;
    MemSet(&p,sizeof(p),0);
    MemSet(&r,sizeof(r),0);
    p.structSize=sizeof(p);
    p.sessionId=n->session;
    p.timeoutTicks=1;
    p.options=PALM_TLS_IO_COOPERATIVE;
    r.structSize=sizeof(r);
    Err e;
    if (n->phase==netHandshake) {
        PalmTlsSessionOpenResult h;
        MemSet(&h,sizeof(h),0);
        h.structSize=sizeof(h);
        e=PalmTlsLibSessionHandshake(n->tls,&p,&h);
        if (!e && h.status==palmTlsStatusWouldBlock) return requestBusy;
        if (e || h.status!=palmTlsStatusOk)
            return Failed(n,"TLS handshake failed",h.tlsError ? (UInt16)-h.tlsError : (e ? e : h.status));
        n->phase=netWrite;
        return requestBusy;
    }
    if (n->phase==netWrite) {
        p.bufferP=n->request+n->written;
        p.length=n->requestLength-n->written;
        e=PalmTlsLibSessionWrite(n->tls,&p,&r);
        if (e || (r.status!=palmTlsStatusOk && r.status!=palmTlsStatusWouldBlock))
            return Failed(n,"Could not send tile request",e ? e : r.status);
        n->written+=(UInt16)r.transferred;
        if (n->written>=n->requestLength) n->phase=netRead;
        return requestBusy;
    }
    p.bufferP=n->chunk;
    p.length=sizeof(n->chunk);
    e=PalmTlsLibSessionRead(n->tls,&p,&r);
    if (e || (r.status!=palmTlsStatusOk && r.status!=palmTlsStatusWouldBlock))
        return Failed(n,"Could not read tile",e ? e : r.status);
    if (r.transferred &&
        HttpParserFeed(&n->parser,n->chunk,(UInt16)r.transferred)!=httpOk)
        return Failed(n,n->parser.statusCode!=200 ? "Tile server refused request" :
            "Invalid or oversized tile",n->parser.statusCode!=200 ? n->parser.statusCode : 0);
    if (n->parser.headersComplete && n->response.blocked)
        return Failed(n,"Tile server refused request",n->parser.statusCode);
    if (n->parser.headersComplete && n->parser.statusCode==304 && n->conditional) {
        MapNetStop(n,true);
        return requestNotModified;
    }
    if (n->parser.headersComplete && n->parser.statusCode!=200)
        return Failed(n,"Tile server refused request",n->parser.statusCode);
    if (!n->parser.connectionComplete && (r.transferred || r.status==palmTlsStatusWouldBlock))
        return requestBusy;
    if (HttpParserFinish(&n->parser)!=httpOk ||
        StrCaselessCompare(n->parser.contentType,"image/png") || !n->length)
        return Failed(n,"Tile is not a PNG",0);
    MapNetStop(n,true);
    return requestReady;
}

static UInt32 Number(const Char *s, const Char *end)
{
    UInt32 n=0;
    while (s<end && *s>='0' && *s<='9') {
        if (n>8640000UL) return 86400000UL;
        n=n*10+(*s++-'0');
    }
    return n;
}

// Expiry zero means no disk storage. A missing lifetime gets the OSM default.
static UInt32 HttpDate(const Char *p, UInt16 length)
{
    static const Char months[]="JanFebMarAprMayJunJulAugSepOctNovDec";
    if (length!=29 || p[3]!=',' || p[4]!=' ' || p[7]!=' ' ||
        p[11]!=' ' || p[16]!=' ' || p[19]!=':' || p[22]!=':' ||
        StrNCompare(p+25," GMT",4)) return 0;
    Int16 month;
    for (month=0;month<12;++month)
        if (!StrNCompare(p+8,months+month*3,3)) break;
    DateTimeType date;
    MemSet(&date,sizeof(date),0);
    date.day=(Int16)Number(p+5,p+7); date.month=month+1;
    date.year=(Int16)Number(p+12,p+16); date.hour=(Int16)Number(p+17,p+19);
    date.minute=(Int16)Number(p+20,p+22); date.second=(Int16)Number(p+23,p+25);
    if (month==12 || date.day<1 || date.day>31 || date.year<1904 ||
        date.year>2039 || date.hour>23 || date.minute>59 || date.second>59 ||
        date.hour<0 || date.minute<0 || date.second<0) return 0;
    return TimDateTimeToSeconds(&date);
}

static Boolean HeaderIs(const Char *name, UInt16 length, const Char *expected)
{
    return length==StrLen(expected) && !StrNCaselessCompare(name,expected,length);
}

static void ReadCacheControl(MapResponseHeaders *headers, const Char *value,
    const Char *end)
{
    while (value<end) {
        while (value<end && (*value==' ' || *value=='\t' || *value==',')) ++value;
        const Char *next=value;
        while (next<end && *next!=',') ++next;
        const Char *nameEnd=value;
        while (nameEnd<next && *nameEnd!='=' && *nameEnd!=' ' && *nameEnd!='\t')
            ++nameEnd;
        UInt16 length=(UInt16)(nameEnd-value);
        if (HeaderIs(value,length,"no-store") || HeaderIs(value,length,"no-cache") ||
            HeaderIs(value,length,"private")) {
            headers->noCache=true;
            return;
        }
        if (HeaderIs(value,length,"max-age")) {
            const Char *seconds=nameEnd;
            while (seconds<next && (*seconds==' ' || *seconds=='\t')) ++seconds;
            UInt32 maxAge=0;
            if (seconds<next && *seconds++=='=') {
                while (seconds<next && (*seconds==' ' || *seconds=='\t')) ++seconds;
                if (seconds<next && *seconds=='"') ++seconds;
                maxAge=Number(seconds,next);
            }
            if (!headers->hasMaxAge || maxAge<headers->maxAge) headers->maxAge=maxAge;
            headers->hasMaxAge=true;
        }
        value=next;
    }
}

static void CollectHeader(void *context, const Char *name, UInt16 nameLength,
    const Char *value, UInt16 valueLength, Boolean overflow)
{
    MapNet *n=context;
    MapResponseHeaders *headers=&n->response;
    if (HeaderIs(name,nameLength,"X-Blocked")) headers->blocked=true;
    else if (HeaderIs(name,nameLength,"Cache-Control")) {
        if (overflow) headers->noCache=true;
        else ReadCacheControl(headers,value,value+valueLength);
    } else if (!overflow) {
        if (HeaderIs(name,nameLength,"Date")) headers->date=HttpDate(value,valueLength);
        else if (HeaderIs(name,nameLength,"Expires")) headers->expires=HttpDate(value,valueLength);
        else if (HeaderIs(name,nameLength,"Age")) {
            UInt32 age=Number(value,value+valueLength);
            if (age>headers->age) headers->age=age;
        }
    }
}

CacheMeta MapNetMetadata(const MapNet *n, UInt32 now)
{
    const MapResponseHeaders *headers=&n->response;
    CacheMeta meta;
    MemSet(&meta,sizeof(meta),0);
    if (headers->noCache) return meta;
    UInt32 ttl=7*86400UL;
    if (headers->hasMaxAge) ttl=headers->maxAge;
    else if (headers->expires && headers->date)
        ttl=headers->expires>headers->date ? headers->expires-headers->date : 0;
    UInt32 fresh=headers->age<ttl ? ttl-headers->age : 0;
    if (fresh>86400000UL) fresh=86400000UL;
    meta.expires=now+fresh;
    if (ValidValidator(n->parser.etag))
        StrCopy(meta.validator,n->parser.etag);
    else if (ValidValidator(n->parser.lastModified)) {
        StrCopy(meta.validator,n->parser.lastModified);
        meta.dateValidator=true;
    }
    return meta;
}
