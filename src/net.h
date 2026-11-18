// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAP_NET_H
#define MAP_NET_H
#include <PalmOS.h>
#include <NetMgr.h>
#include "http.h"
#include "map.h"

enum { netIdle, netDns, netConnect, netConnecting, netTls, netHandshake, netWrite, netRead };
enum { requestBusy, requestReady, requestNotModified, requestError };
#define MAP_BODY_MAX (256UL * 1024)

typedef struct {
    UInt32 age, date, expires, maxAge;
    Boolean hasMaxAge, noCache, blocked;
} MapResponseHeaders;

typedef struct {
    Boolean tlsOpen, tlsLoaded, netOpen, netLoaded, conditional, vector;
    UInt16 tls, net, phase, code, protocol;
    UInt32 options, session, deadline, length;
    UInt16 requestLength, written;
    NetSocketRef socket;
    NetSocketAddrINType address;
    NetHostInfoBufType dns;
    HttpUrl url;
    HttpParser parser;
    MapResponseHeaders response;
    Char request[768];
    UInt8 chunk[2048];
    UInt8 *body;
    MemHandle roots;
    const UInt8 *trust;
    UInt32 trustSize;
    const Char *error;
} MapNet;

Boolean MapNetStart(MapNet *n, const Char *url, const CacheMeta *cached, Boolean forceTls12, Boolean vector);
UInt16 MapNetStep(MapNet *n);
void MapNetStop(MapNet *n, Boolean completed);
void MapNetClose(MapNet *n);
CacheMeta MapNetMetadata(const MapNet *n, UInt32 now);
#endif
