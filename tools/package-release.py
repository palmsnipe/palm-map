#!/usr/bin/env python3
"""Package a clean Map build alongside the verified PalmTLS release."""

import argparse
import hashlib
import io
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_source(repository, commit, destination):
    subprocess.run(
        ["curl", "--fail", "--location", "--silent", "--show-error", "--retry", "3",
         f"https://codeload.github.com/{repository}/tar.gz/{commit}",
         "--output", destination],
        check=True,
    )


def add_tree(source, destination, prefix, strip_root=False):
    for member in source:
        name = member.name.split("/", 1)[-1] if strip_root else member.name
        if strip_root and name == member.name:
            continue
        member.name = f"{prefix}/{name}"
        member.pax_headers = {
            key: value for key, value in member.pax_headers.items()
            if key not in ("path", "linkpath")
        }
        if member.islnk():
            target = member.linkname.split("/", 1)[-1] if strip_root else member.linkname
            member.linkname = f"{prefix}/{target}"
        destination.addfile(member, source.extractfile(member) if member.isfile() else None)


def write_source_archive(path, prefix, archives, pins):
    instructions = f"""Map {prefix.removeprefix('Map-').removesuffix('-source')} corresponding source

map/ contains the exact Map source and assets for this build. See map/README.md.
palm-tls/ contains PalmTLS {pins['version']} ({pins['palmtls_commit']}).
wolfssl/ contains the pinned wolfSSL source ({pins['wolfssl_commit']}).
The PalmTLS patches are in palm-tls/patches/wolfssl. Its bootstrap script
fetches the pinned wolfSSL revision and applies these patches.
The packaged {pins['asset']} is the verified upstream release asset.

Install Palm Toolchain and a user-supplied Palm OS 5r4 SDK. Set
PALM_TOOLCHAIN_PREFIX to the installed toolchain prefix and run

  make -C palm-tls bootstrap
  make -C palm-tls/library TLS_PROFILE=modern PALM_TLS_TEST_RANDOM=0 test-prc
  make -C map PALM_TOOLCHAIN_ROOT=/path/to/palm-toolchain test
""".encode()

    with tarfile.open(path, "w:gz") as archive:
        for label, source in archives.items():
            with tarfile.open(source) as tree:
                add_tree(tree, archive, f"{prefix}/{label}", strip_root=label != "map")
        build_notes = tarfile.TarInfo(f"{prefix}/BUILD.txt")
        build_notes.size = len(instructions)
        archive.addfile(build_notes, io.BytesIO(instructions))


def write_install_zip(path, app, library, source_name, palmtls, wolfssl, pins):
    files = [ROOT / name for name in (
        "README.md", "LICENSE", "THIRD_PARTY.md", "vendor/STARTER-MIT", "vendor/zlib/LICENSE"
    )]
    files.extend(sorted((ROOT / "screenshots").glob("*.png")))

    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for file in files:
            archive.write(file, file.relative_to(ROOT))
        for file in (app, library):
            archive.write(file, file.name)
        with tarfile.open(palmtls) as source:
            prefix = f"palm-tls-{pins['palmtls_commit']}"
            for name, member in (
                ("PalmTLS-LICENSE.txt", "LICENSE"),
                ("PalmTLS-THIRD_PARTY.md", "THIRD_PARTY.md"),
                ("P256-BSD-3-Clause.txt", "library/arm/p256-inverse.LICENSE"),
            ):
                archive.writestr(f"licenses/{name}", source.extractfile(f"{prefix}/{member}").read())
        with tarfile.open(wolfssl) as source:
            member = f"wolfssl-{pins['wolfssl_commit']}/COPYING"
            archive.writestr("licenses/wolfSSL-COPYING.txt", source.extractfile(member).read())
        archive.writestr("SOURCE.txt", f"Corresponding source is in {source_name}, available from the same release.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", help="Verify the release tag against Map's resource version")
    args = parser.parse_args()

    resource = (ROOT / "resources/app.rcp").read_text()
    version = re.search(r'^VERSION ID 1000 "(\d+\.\d+\.\d+)"$', resource, re.MULTILINE)
    if version is None:
        parser.error("Map's version resource is missing or invalid")
    version = version[1]
    if args.tag is not None and args.tag != f"v{version}":
        parser.error(f"tag {args.tag} does not match Map version {version}")
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT):
        parser.error("commit the Map sources before packaging a release")

    pins = dict(line.split("=", 1) for line in (ROOT / "tools/palmtls-release.env").read_text().splitlines())
    app = BUILD / "Map.prc"
    library = BUILD / pins["asset"]
    if not app.is_file() or not library.is_file():
        parser.error("run make test and tools/fetch-palmtls.sh before packaging")
    if sha256(library) != pins["sha256"]:
        parser.error(f"{library.name} does not match the pinned PalmTLS release digest")

    package = f"Map-{version}"
    release = BUILD / "release"
    with tempfile.TemporaryDirectory(dir=BUILD) as directory:
        staging = Path(directory)
        archives = {
            "map": staging / "map.tar",
            "palm-tls": staging / "palm-tls.tar.gz",
            "wolfssl": staging / "wolfssl.tar.gz",
        }
        subprocess.run(["git", "archive", "--output", archives["map"], "HEAD"], cwd=ROOT, check=True)
        fetch_source("palmsnipe/palm-tls", pins["palmtls_commit"], archives["palm-tls"])
        fetch_source("wolfSSL/wolfssl", pins["wolfssl_commit"], archives["wolfssl"])

        source_prefix = f"{package}-source"
        source = staging / f"{source_prefix}.tar.gz"
        install = staging / f"{package}.zip"
        write_source_archive(source, source_prefix, archives, pins)
        write_install_zip(install, app, library, source.name, archives["palm-tls"], archives["wolfssl"], pins)
        staged_assets = [shutil.copyfile(file, staging / file.name) for file in (app, library)]
        staged_assets.extend((install, source))
        sums = staging / "SHA256SUMS"
        sums.write_text("".join(f"{sha256(asset)}  {asset.name}\n" for asset in staged_assets))
        release.mkdir(exist_ok=True)
        for asset in (*staged_assets, sums):
            asset.replace(release / asset.name)

    print(f"Packaged Map {version} in {release}")


if __name__ == "__main__":
    main()
