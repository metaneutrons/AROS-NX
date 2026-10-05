#!/usr/bin/env python3
"""Make a kickstart member's shared symbols file-local.

config/make.tmpl:2760 does this with a shell backquote:

    $(OBJCOPY) $@ $(FILTBASES) `$(NM_PLAIN) $@ \
        | $(AWK) '($$3 ~ /^__.*_(LIST|END)__\\r?$$/) \
                  || ($$3 ~ /^__aros_lib.*\\r?$$/) {print "-L " $$3;}'`

The library bases are known up front and come in as arguments. The set-list
symbols are named per module, so they have to be read back out of the linked
object, which is what this does. Localising them is what lets several members be
linked into one image: without it the joint link fails with
`duplicate symbol: set_call_libfuncs`.

Usage: localise-symbols.py <objcopy> <nm> <object> [--localize-symbol=NAME ...]
"""

from __future__ import annotations

import re
import subprocess
import sys

# The two patterns the reference awk script matches. `\r?` is in the original
# because the tools may be Windows builds; kept for the same reason.
SET_LIST = re.compile(r"^__.*_(?:LIST|END)__\r?$")
AROS_LIB = re.compile(r"^__aros_lib.*\r?$")


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    objcopy, nm, obj = sys.argv[1:4]
    explicit = sys.argv[4:]

    listing = subprocess.run(
        [nm, "--format=posix", obj],
        capture_output=True,
        text=True,
        check=True,
    ).stdout

    found: list[str] = []
    for line in listing.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[0]
        if SET_LIST.match(name) or AROS_LIB.match(name):
            if name not in found:
                found.append(name)
    found.sort()

    args = list(explicit) + [f"--localize-symbol={name}" for name in found]
    if not args:
        # Nothing to localise is a legitimate outcome for a module with no
        # bases and no sets; the object stays as linked.
        return 0
    subprocess.run([objcopy, *args, obj], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
