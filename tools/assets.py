#!/usr/bin/env python3
"""Build Palm resources from public-domain Natural Earth and CC BY GeoNames."""
import argparse
import json
import math
import pathlib
import struct
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORLD = 1 << 28

def project(lat, lon):
    lat = max(-85.05112878, min(85.05112878, lat))
    return ((lon + 180) / 360, (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2)

def magick(*args):
    subprocess.run(["magick", *map(str, args)], check=True)

def cities(zip_path, out):
    fields = [line.split("\t") for line in
              zipfile.ZipFile(zip_path).read("cities15000.txt").decode().splitlines()]
    fields.sort(key=lambda f: -int(f[14]))
    count, chunk, names = 0, bytearray(), []
    for f in fields:
        name = f[2].encode("ascii", errors="ignore")[:43]
        if not name or any(c < 32 for c in name):
            continue
        x, y = project(float(f[4]), float(f[5]))
        row = struct.pack(">II", int(x*WORLD) % WORLD, max(0, min(WORLD-1, int(y*WORLD))))
        row += name + b", " + f[8].encode("ascii") + b"\x00"
        if len(chunk) + len(row) > 24000:
            (out / f"cities{count}.bin").write_bytes(chunk)
            names.append(f'DATA "city" ID {1000+count} "cities{count}.bin"')
            count += 1
            chunk.clear()
        chunk.extend(row)
    if chunk:
        (out / f"cities{count}.bin").write_bytes(chunk)
        names.append(f'DATA "city" ID {1000+count} "cities{count}.bin"')
    return names

def overview(source, out):
    features = json.loads(source.read_text())["features"]
    size = 2048
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}">',
           f'<rect width="{size}" height="{size}" fill="#b6dce3"/>',
           '<g fill="#eee9d3" stroke="#a8ab91" stroke-width="1" fill-rule="evenodd">']
    for f in features:
        geom = f["geometry"]
        for polygon in (geom["coordinates"] if geom["type"] == "MultiPolygon" else [geom["coordinates"]]):
            rings = []
            for ring in polygon:
                pts = [project(lat, lon) for lon, lat, *_ in ring]
                rings.append("M" + " ".join(f"{x*size:.1f},{y*size:.1f}" for x,y in pts) + "Z")
            svg.append('<path d="' + "".join(rings) + '"/>')
    svg.append("</g></svg>")
    with tempfile.TemporaryDirectory() as tmp:
        drawing = pathlib.Path(tmp) / "atlas.svg"
        atlas = pathlib.Path(tmp) / "atlas.png"
        drawing.write_text("\n".join(svg))
        magick(drawing, atlas)
        for z in range(4):
            n = 256 << z
            magick(atlas, "-resize", f"{n}x{n}!", "-crop", "256x256", "+repage",
                   "-colors", "48", "-strip", f"PNG8:{out}/world{z}-%d.png")
    return [f'DATA "wrld" ID {1000+(4**z-1)//3+i} "world{z}-{i}.png"'
            for z in range(4) for i in range(4**z)]

def icons(out):
    for name, size, canvas in (("icon", "27x22!", "32x22"), ("icon2x", "54x44!", "64x44"),
                                ("small", "13x9!", "15x9"), ("small2x", "26x18!", "30x18")):
        magick("-background", "none", f"MSVG:{ROOT / 'resources/map.svg'}", "-filter", "point",
               "-resize", size, "-background", "#ff00ff", "-alpha", "remove",
               "-gravity", "center", "-extent", canvas, f"BMP3:{out}/{name}.bmp")

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=pathlib.Path)
    p.add_argument("countries", type=pathlib.Path)
    p.add_argument("cities", type=pathlib.Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    icons(a.output)
    entries = cities(a.cities, a.output) + overview(a.countries, a.output)
    (a.output / "generated.rcp").write_text("\n".join(entries) + "\n")

if __name__ == "__main__":
    main()
