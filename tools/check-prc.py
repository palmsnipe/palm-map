#!/usr/bin/env python3
"""Check Map's native code, launcher art and offline datasets are packaged."""
import pathlib
import struct
import sys

prc, arm = (pathlib.Path(a).read_bytes() for a in sys.argv[1:])
count = int.from_bytes(prc[76:78], "big")
entries = [struct.unpack_from(">4sHI", prc, 78+10*i) for i in range(count)]
resources = {(typ, idx): prc[start:entries[i+1][2] if i+1<count else len(prc)]
             for i, (typ, idx, start) in enumerate(entries)}
for key in [(b"tAIB", 1000), (b"tAIB", 1001), (b"tver", 1000),
            (b"tFRM", 1010), (b"tFRM", 1020), (b"pref", 0), (b"cert", 1000)]:
    assert resources.get(key), key
assert resources[b"armc", 1000] == arm and 1000 < len(arm) < 65536
assert not any(typ == b"tRng" for typ, _ in resources)
assert sum(typ == b"wrld" for typ, _ in resources) == 85
assert sum(len(data) for (typ,_),data in resources.items() if typ == b"city") > 700000
print("Validated ARMlet, launcher icons, forms, overview, city index and absence of seed resource")
