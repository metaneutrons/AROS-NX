#!/usr/bin/env python3
"""
Compare a mounted image against the manifest the generator wrote.

Host artefacts are ignored, and only the ones a mount is known to create:
.fseventsd, .Spotlight-V100, .Trashes, ._* resource forks and .DS_Store.
Anything else that is present but not in the manifest is reported, because an
unexplained extra entry is exactly what this check exists to catch.
"""

import hashlib
import os
import sys

HOST_NOISE = (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems",
              ".DS_Store", ".apdisk")


def is_noise(name):
    return name in HOST_NOISE or name.startswith("._")


def main():
    if len(sys.argv) != 3:
        print("usage: check-manifest.py MOUNTPOINT MANIFEST", file=sys.stderr)
        return 2
    mp, manifest = sys.argv[1], sys.argv[2]

    want = {}
    for line in open(manifest):
        p = line.split()
        if not p:
            continue
        if p[0] == "dir":
            want[p[1]] = ("dir", None, None)
        else:
            want[p[1]] = ("file", int(p[2]), p[3])

    have = {}
    for root, dirs, files in os.walk(mp):
        dirs[:] = [d for d in dirs if not is_noise(d)]
        rel = "" if root == mp else "/" + os.path.relpath(root, mp)
        for d in dirs:
            have[rel + "/" + d] = ("dir", None, None)
        for f in files:
            if is_noise(f):
                continue
            with open(os.path.join(root, f), "rb") as fh:
                data = fh.read()
            have[rel + "/" + f] = ("file", len(data),
                                   hashlib.sha256(data).hexdigest())

    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    differing = sorted(k for k in set(want) & set(have) if want[k] != have[k])

    print("manifest entries %d, mounted entries %d" % (len(want), len(have)))
    for k in missing:
        print("  missing from image: %s" % k)
    for k in extra:
        print("  present but not in manifest: %s" % k)
    for k in differing:
        print("  differs: %s manifest=%s image=%s" % (k, want[k], have[k]))

    if missing or extra or differing:
        print("MANIFEST MISMATCH")
        return 1
    print("manifest matches the mounted image exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
