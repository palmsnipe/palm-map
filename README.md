# Map for Palm OS 5

Map is a raster OpenStreetMap viewer for Palm OS 5 on ARM handhelds. It uses
Palm's native forms through PACE; PNG decoding, tile color conversion and
coordinate projection run in an ARMv4T ARMlet. Fixed Graffiti areas and
Palm OS dynamic input areas both work.

Install `build/Map.prc` with HotSync, from a card, or in CloudpilotEmu.
Open **Map**. Drag with the stylus, pan with the directional buttons, or use
**+** and **−** in the lower right of the map to change zoom. **Map → Find**
searches over 34,000 cities offline. Try `Montreal` or `45.5017, -73.5673`,
select a result, and tap **Show on map**. Open the **Layers** menu and tap a layer to switch.

- **Streets**: the standard OpenStreetMap raster style, zoom 0–19.
- **Night**: a darker rendering of the same OpenStreetMap tiles. Shares the
  tile cache with Streets.
- **Overview**: a built-in Natural Earth map, zoom 0–3, usable without a connection.
- **Custom**: set a tile template under **Map → Custom tile layer**. Use an
  authorized provider or your own server, with an HTTPS URL such as
  `https://maps.example.org/{z}/{x}/{y}.png`, non-interlaced 256 × 256 PNG tiles,
  and a domain certificate trusted by the bundled GlobalSign/ISRG roots.

Map opens on Streets and restores the saved viewport and marker. The tile cache is bounded
to about 3 MiB and uses server expiry and validators. Map only requests tiles
visible after a pan or zoom; it does not bulk download tiles for offline use.
See the [standard tile policy](https://operations.osmfoundation.org/policies/tiles/).
To retry after connecting, tap the footer or use **Map → Retry tiles**.

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
PNG, corrupt tiles, the bundled atlas and Mercator projection. The checked-in
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
