# Map for Palm OS 5

Map is an OpenStreetMap viewer for Palm OS 5 on ARM handhelds. It uses
Palm's native forms through PACE. PNG decoding, vector rendering and
coordinate projection run in an ARMv4T ARMlet. Fixed Graffiti areas and
Palm OS dynamic input areas both work.

Install `build/Map.prc` with HotSync, from a card, or in CloudpilotEmu.
Open **Map**. Drag with the stylus, pan with the directional buttons, or use
**+** and **−** in the lower right of the map to change zoom. **Map → Find**
searches over 34,000 cities offline. Try `Montreal` or `45.5017, -73.5673`,
select a result, and tap **Show on map**. Exact city names appear first.
Use **Map → Go to pin** to return to the marker at the current zoom.
**Map → Clear pin** removes the marker in place.
Open the **Layers** menu and tap a layer to switch.

- **Streets**: the standard OpenStreetMap raster style, zoom 0–19.
- **Night**: a darker rendering of the same OpenStreetMap tiles. Shares the
  tile cache with Streets.
- **Cycle Map**: CyclOSM raster tiles with cycling routes and infrastructure,
  served over HTTPS by OpenStreetMap France. See the [CyclOSM map](https://www.cyclosm.org/)
  and the [OpenStreetMap France usage policy](https://www.openstreetmap.fr/usage/).
- **Overview**: a built-in Natural Earth map, zoom 0–3, usable without a connection.
- **Custom**: set a tile template under **Map → Custom tile layer**. Use an
  authorized provider or your own server, with an HTTPS URL such as
  `https://maps.example.org/{z}/{x}/{y}.png`, non-interlaced 256 × 256 PNG tiles,
  and a domain certificate trusted by the bundled GlobalSign/ISRG roots.
- **Shortbread**: live OpenStreetMap vector tiles with a compact style for
  land, water, borders, streets and building footprints. City names appear
  from zoom 4, water names from zoom 5, and street names from zoom 10 where
  supplied. Named landmarks such as towers, museums, and
  historic sites appear from zoom 14. Labels use Palm font glyphs at native
  pixel size. Street names follow their road segment, and landmark names
  stay horizontal. Labels avoid other labels, the search pin, and the zoom
  buttons. At display zooms 15–19, the app reuses a level 14 source tile and
  its label index.
  See the [Shortbread schema](https://shortbread-tiles.org/schema/1.0/)
  and the [OSM vector tile policy](https://operations.osmfoundation.org/policies/vector/).

Open **Layers → Shortbread settings** to show or hide roads, buildings,
water, land cover, city and water names, street names, and landmarks.
Provincial and national borders have separate switches. Turn off **Angle
streets** to draw road labels horizontally, or enable high contrast for more
distinct roads and features. **Apply** saves your choices.
**Defaults** restores the original selections in the form.

Map opens on Streets and restores the saved viewport and marker. The tile cache is bounded
to about 3 MiB and uses server expiry and validators. Map only requests tiles
visible after a pan or zoom; it does not bulk download tiles for offline use.
See the [standard tile policy](https://operations.osmfoundation.org/policies/tiles/).
To retry after connecting, tap the footer or use **Map → Retry tiles**.

The top-right counter shows tile data downloaded in the current launch in
decimal kilobytes. It counts compressed tiles as received and stays unchanged
when you use cached tiles. **Map → Offline mode** stops new tile downloads
until you select **Online**. Cached tiles and the bundled Overview remain
available, and the counter shows **Off** while offline mode is active.
For raster layers, Map can enlarge tiles cached up to two zoom levels
out. These views are marked **Cached preview**.

## Enable HTTPS

Map defaults to **TLS 1.2** for tile requests and remembers any mode you
previously selected in **Map → HTTPS mode**.

### Set up the default TLS 1.2 mode

1. Install `build/PalmTLSArm-TLS12-Compat.prc` in place of the Browser profile.
   This build uses the [PalmTLS 0.1.9 source](https://github.com/palmsnipe/palm-tls/tree/v0.1.9)
   and adds SHA-384 and 4096-bit RSA support to the standalone TLS 1.2 engine.
   The upstream ARM Modern download lacks that hash in its TLS 1.2 engine,
   so it cannot verify OpenStreetMap's current certificate chain.
   Install one PalmTLS profile at a time; they use the same database name.
2. Install the current `Map.prc` and open Map. The default applies automatically.
   If you saved a Modern preference, choose **Map → HTTPS mode → TLS 1.2** once.
   This mode uses TLS 1.2 for every request and verifies the server certificate and hostname.

The standalone engine generates its own randomness from Palm OS `SysRandom`
and device timing. This is weaker than Modern's separately provisioned
randomness. Use compatibility mode for public map tiles and keep it off
for custom tile services that require secrets.

### Modern (recommended)

1. Install `build/PalmTLSArm-Browser.prc`, the negotiated HTTPS profile built
   from the exact [PalmTLS 0.1.9 source tag](https://github.com/palmsnipe/palm-tls/tree/v0.1.9).
   Install only one PalmTLS library. The published 0.1.9
   `PalmTLSArm-Modern.prc` contains the older separate TLS engines and does
   not expose the negotiated, CA-verified HTTPS API this app uses.
2. Set the Palm's date/time and configure an IPv4 connection.
3. Provision PalmTLS independently. A trusted host can generate fresh
   randomness and a separate tool such as Palm Browser can import it; see
   [Palm Browser's setup](https://github.com/palmsnipe/palm-browser#enable-https-after-boot).
   Alternatively, the [PalmTLS Tester](https://github.com/palmsnipe/palm-tls/tree/v0.1.9/apps/tls-tester)
   offers an experimental stylus source on supported handhelds. In TLS Tester
   select **HTTPS / Stylus → Seed**, follow the prompt, and tap **Use seed**.
4. Open **Map → HTTPS mode** and choose **Modern**. A soft reset clears PalmTLS's boot-scoped
   randomness; provision it again after a reset or if PalmTLS reports that
   it needs entropy.

Map contains no seed, never imports one, and never gathers stylus samples
for cryptography. Modern mode uses a ready host-provisioned source first, or
the ready stylus source if explicitly provisioned in TLS Tester. It verifies HTTPS
certificates and hostnames and does not fall back to plaintext.

## Build

With the [Palm Toolchain](https://github.com/palmsnipe/palm-toolchain) and a
user-supplied SDK 5r4 installed, plus Node, Python 3, and host zlib headers:

```sh
make PALM_TOOLCHAIN_ROOT=../palm-toolchain test
```

This builds `build/Map.prc`, checks the package and runs host tests for indexed
PNG, corrupt tiles, the bundled atlas, Mercator projection, gzip, vector overzoom, indexed labels and rotated text. The checked-in
map assets are ready to build. To regenerate them, download the sources from
[THIRD_PARTY.md](THIRD_PARTY.md), install ImageMagick 7, and run
`python3 tools/assets.py data /path/to/countries.geojson /path/to/cities15000.zip`.

To rebuild the separate TLS PRC, bootstrap the wolfSSL dependencies in a
PalmTLS v0.1.9 checkout, then run `tools/build-palmtls.sh` from this project
(override `PALM_TLS_ROOT` if it isn't next to this project). The helper
builds the tagged source in this project's build directory with the
PalmTLS test seed disabled. Run `tools/build-palmtls-tls12.sh` with the
same prerequisites to rebuild the TLS 1.2 compatibility profile.

Map's source is GPL-3.0-or-later. See [THIRD_PARTY.md](THIRD_PARTY.md) for
data and dependency attribution.

## Verification

`make test` passed. In CloudpilotEmu, Map was installed and exercised on a
Tungsten E2 (Palm OS 5.4.7, fixed input area) and a Palm TX (5.4.9, DIA).
Checks covered live OpenStreetMap HTTPS rendering, switching to Night,
pan and zoom, offline city and coordinate lookup, and resizing with the
TX input area open and closed. An HTTPS fixture served 99,790-byte tiles
with an ETag; after relaunch it received conditional GETs and returned
304 while Map displayed the tiles from disk. Physical hardware was not tested.

Map 0.1.1's TLS 1.2 compatibility mode was also verified on the Tungsten E2
with no provisioned seed: live OpenStreetMap tiles loaded at zoom levels
3 and 4, and the mode persisted across relaunch.

For Map 0.1.2, both emulated devices loaded OpenStreetMap tiles using the
default TLS 1.2 setting. The E2 check covered the Find and Layers menu items
and an update from 0.1.1 with a saved layer, zoom level, and marker. The TX
check covered both zoom controls with the input area closed and checked their
position with the input area open.

Map 0.1.6 loaded live Shortbread tiles over TLS 1.2 on CloudpilotEmu's
Tungsten E2 at the default 100 MIPS. At Parliament Hill, the ARMlet took an
average of 200 ms per tile at zoom 14 (four tiles, 260 ms maximum) and
80 ms per tile at zoom 19 (four tiles, 80 ms maximum). The zoom 19 pass
reused a decompressed source tile in RAM. These timings cover drawing the
256 × 256 tiles in the ARMlet. The benchmark measures decompression separately
and excludes download, bitmap allocation and screen updates. To reproduce the
timing overlay, build with `make BUILD_DIR="$PWD/build/vector-bench" VECTOR_BENCH=1`.

Map 0.1.7 was tested at the same Parliament Hill coordinates on the
emulated Tungsten E2 at 100 MIPS. Times are averages over four 256 × 256
tiles at each zoom. Label selection and painting are measured once per
viewport redraw, separately from the ARMlet.

| Zoom | Geometry in the ARMlet | Label extraction in the ARMlet | Viewport labels |
| --- | ---: | ---: | ---: |
| 14 | 202 ms/tile | 52 ms/tile | 20 ms, 8 labels |
| 19 | 87 ms/tile | 42 ms/tile | 0 ms, no anchors in the centered view |

Panning toward Peace Tower at zoom 19 rendered its name and point marker.
Palm OS reports timings in 10 ms ticks. Network transfer and screen
blitting are outside these timings.

Map 0.1.8 repeated the test at Parliament Hill on the emulated Tungsten E2
at 100 MIPS. Four visible tiles were timed at each zoom. Times use Palm OS
ticks at 100 Hz.

| Zoom | Geometry | Label extraction | Final viewport label pass |
| --- | ---: | ---: | ---: |
| 14 | 165 ms/tile | 35 ms/tile | 20 ms for eight labels |
| 19 | 80 ms/tile | Under 10 ms/tile | 10 ms for one label after panning to Peace Tower |

The first overzoom built the label index for the Parliament Hill source tile
in 80 ms. The zoom 19 tile measurement reused that index. Tile download,
inflation, and screen bitmap blitting were measured separately or outside
these figures.

Map 0.2.1 was checked on the emulated Tungsten E2 at 100 MIPS. Four
CyclOSM tiles at zoom 14 averaged 52 ms each to decode. The Shortbread
geometry pass at Parliament Hill averaged 10 ms per tile at zoom 19
with the source tile and its path index already in memory. The download
counter increased on live requests and held steady when revisiting cached
tiles and overzooming the same vector source.

Map 0.2.5 was benchmarked at Parliament Hill on the emulated Tungsten E2
at 100 MIPS with all four geography options on. Processing four display
tiles took about 1,040 ms at zoom 14 and 40 ms at zoom 19 with the decoded
source and indexes in RAM. At zoom 19, decompressing and indexing the
cached source added about 150 ms. The timings include geometry, label
extraction, and the final viewport label pass. Network wait and terrain
bitmap blitting are outside these timings. The overlays use the same tile
payload as the rest of Shortbread.
