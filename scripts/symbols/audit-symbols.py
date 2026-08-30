#!/usr/bin/env python3
"""Report undefined symbols in the built AROS modules.

Why this exists
---------------

Every link in this build is `ld.lld -r`, partial. A missing link library or a
missing implementation therefore never fails a link: it produces a relocatable
object with dangling externals that fails when AROS loads it. A green build
says nothing about whether a module can be loaded, and nothing else here
measures that.

The contract being checked
--------------------------

A loadable AROS module must have no strong undefined symbols. AROS does not
resolve anything by ELF symbol name when it loads a module: a call to another
library goes through a stub that genmodule writes into that module's link
library, and the stub reaches the callee through its library base. So the stubs
have to be linked in, and what is left undefined afterwards is what will fail.

Calibration, because the obvious model is wrong: kernel-exec.library itself
lists `AllocMem` as undefined. exec does not export its API as global symbols;
the entry points live in a function table. Reading "AllocMem is undefined in
242 modules" as "exec is missing" would therefore be a misreading. The correct
reading is that those 242 modules have no exec stubs linked in.

What it reports, and why in this shape
--------------------------------------

Three groupings, because the raw count answers no question:

  by symbol     One missing link library shows up as one line, not as the
                several hundred modules that reference it. `CloseLibrary`
                undefined in 431 places is one cause.
  by provider   Whether any built artefact defines the symbol at all. Some
                artefact defines it means the link was simply not made; nothing
                defines it means its provider -- usually a genmodule stub or a
                link library -- has not been built yet. The two need different
                work, and the second is the larger group here.
  by module     Which modules are worst, for picking a starting point.

Weak undefined symbols are counted separately and not treated as defects: they
resolve to zero by definition.

The gate is a ratchet, not a wall. The current total is far from zero, so
demanding zero would make the check useless on the first day. Instead the
baseline is pinned and the check fails when a number rises. That turns a
quantity nobody can fix at once into one that cannot silently grow.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import shutil
import subprocess
import sys

ELF_MAGIC = b"\x7fELF"


def find_elf(root: pathlib.Path) -> list[pathlib.Path]:
    """Every ELF file under root, in a stable order."""
    out = []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        try:
            with p.open("rb") as f:
                if f.read(4) == ELF_MAGIC:
                    out.append(p)
        except OSError:
            continue
    return out


def declared_artefacts(manifest: pathlib.Path | None) -> set[pathlib.Path] | None:
    """The artefacts this configuration produces, or None if not declared.

    Nothing removes an artefact whose output name changed, so a tree that has
    been reconfigured carries orphans: SYS/Libs/muimaster.library alongside
    SYS/Libs/workbench-libs-muimaster.library from an earlier scheme. Measuring
    both counts the module twice, and the stale copy is a snapshot of whatever
    the build did when it was written, which makes the metric untrustworthy.
    """
    if manifest is None or not manifest.is_file():
        return None
    declared = set()
    for line in manifest.read_text().splitlines():
        line = line.strip()
        if line:
            declared.add(pathlib.Path(line).resolve())
    return declared or None


def symbols(nm: str, path: pathlib.Path) -> tuple[set[str], set[str], set[str]]:
    """(strong undefined, weak undefined, defined) for one object.

    `llvm-nm --format=posix` prints "<name> <type> [value] [size]". Undefined is
    U, weak-undefined is w, and anything else is a definition this object can
    satisfy for another.
    """
    r = subprocess.run(
        [nm, "--format=posix", "--no-sort", str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    strong, weak, defined = set(), set(), set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name, kind = parts[0], parts[1]
        if kind == "U":
            strong.add(name)
        elif kind == "w":
            weak.add(name)
        else:
            defined.add(name)
    return strong, weak, defined


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, type=pathlib.Path,
                    help="directory holding the built artefacts, e.g. <build>/SYS")
    ap.add_argument("--nm", required=True, help="llvm-nm to use")
    ap.add_argument("--report-dir", required=True, type=pathlib.Path)
    ap.add_argument("--libbases", type=pathlib.Path,
                    help="file of library base names, one per line, as written "
                         "by `aros-genmodule --output-libbases`")
    ap.add_argument("--load-sets", type=pathlib.Path,
                    help="tab-separated load sets: <name> then the member "
                         "paths, as written by cmake/SymbolAudit.cmake from "
                         "aros_record_load_set()")
    ap.add_argument("--artefacts", type=pathlib.Path,
                    help="artefacts this configuration produces, one path per "
                         "line; anything else under --root is an orphan and is "
                         "reported rather than measured")
    ap.add_argument("--baseline", type=pathlib.Path,
                    help="pinned counts; the check fails when a number rises")
    ap.add_argument("--update-baseline", action="store_true",
                    help="write the measured counts to --baseline and exit 0")
    args = ap.parse_args()

    if not pathlib.Path(args.nm).is_file() and not shutil.which(args.nm):
        print(f"symbol audit: no usable llvm-nm at {args.nm}", file=sys.stderr)
        return 2
    if not args.root.is_dir():
        print(f"symbol audit: no such directory: {args.root}", file=sys.stderr)
        return 2

    objects = find_elf(args.root)
    declared = declared_artefacts(args.artefacts)
    orphans: list[pathlib.Path] = []
    if declared is not None:
        kept = []
        for path in objects:
            if path.resolve() in declared:
                kept.append(path)
            else:
                orphans.append(path)
        objects = kept
    if not objects:
        print(f"symbol audit: no ELF artefacts under {args.root}", file=sys.stderr)
        return 2

    root = args.root.resolve()
    per_module: dict[str, set[str]] = {}
    weak_per_module: dict[str, set[str]] = {}
    defined_by: dict[str, set[str]] = {}
    provider: set[str] = set()
    for obj in objects:
        rel = str(obj.relative_to(args.root))
        strong, weak, defined = symbols(args.nm, obj)
        per_module[rel] = strong
        weak_per_module[rel] = weak
        defined_by[rel] = defined
        provider |= defined

    # Artefacts that are relocated as one unit may call into each other. A
    # kickstart member referring to another member is satisfied when the
    # kickstart is linked, so demanding that every artefact be self-contained
    # reported those as gaps. rom/intuition declares no uselibs and the
    # reference links no C library into it either: its calls into the rest of
    # the kickstart are correct, not missing.
    #
    # A member keeps whatever its own set cannot supply. An artefact in no set
    # is still required to be self-contained, which is the right contract for a
    # separately loaded module.
    load_sets: dict[str, list[str]] = {}
    kickstart_members: set[str] = set()
    if args.load_sets and args.load_sets.is_file():
        for line in args.load_sets.read_text().splitlines():
            fields = [f for f in line.split("\t") if f.strip()]
            if len(fields) < 3:
                continue
            kind, name, members = fields[0], fields[1], fields[2:]
            rel = []
            for m in members:
                try:
                    rel.append(str(pathlib.Path(m).resolve().relative_to(root)))
                except ValueError:
                    continue
            if rel:
                load_sets[name] = rel
                if kind == "kickstart":
                    kickstart_members.update(rel)

    set_resolved = 0
    for name, members in load_sets.items():
        provided_in_set: set[str] = set()
        for m in members:
            if m in defined_by:
                provided_in_set |= defined_by[m]
        for m in members:
            if m not in per_module:
                continue
            resolvable = per_module[m] & provided_in_set
            if resolvable:
                set_resolved += len(resolvable)
                per_module[m] = per_module[m] - provided_in_set

    # What the loaders actually forgive, read from their sources rather than
    # assumed. I had excused every library base here on the belief that the
    # loader fills them in. It does not.
    #
    #   bootstrap/elfloader.c:157   a kickstart member may leave `SysBase`
    #                               undefined; the loader substitutes the
    #                               default base. Any other undefined symbol
    #                               prints "Undefined symbol" and fails.
    #   rom/dos/internalloadseg_elf.c:509
    #                               a separately loaded module may leave
    #                               nothing undefined. Every one fails with
    #                               ERROR_BAD_HUNK, SysBase included.
    #
    # So the only expected undefined symbol in the whole tree is SysBase, and
    # only in a kickstart member. DOSBase, IntuitionBase and UtilityBase
    # undefined are real defects, which the earlier blanket rule hid.
    libbases: set[str] = set()
    if args.libbases and args.libbases.is_file():
        libbases = {
            line.strip()
            for line in args.libbases.read_text().splitlines()
            if line.strip()
        }

    expected: dict[str, set[str]] = {}
    for module in list(per_module):
        if module in kickstart_members and "SysBase" in per_module[module]:
            expected[module] = {"SysBase"}
            per_module[module] = per_module[module] - {"SysBase"}

    by_symbol: collections.Counter[str] = collections.Counter()
    for syms in per_module.values():
        by_symbol.update(syms)
    expected_refs = sum(len(v) for v in expected.values())

    # Defined by some other artefact means a link nobody made. Defined nowhere
    # means an implementation nobody wrote. The two need different work, so they
    # are counted apart.
    unlinked = {s: c for s, c in by_symbol.items() if s in provider}
    absent = {s: c for s, c in by_symbol.items() if s not in provider}

    measured = {
        "artefacts": len(objects),
        "modules_with_undefined": sum(1 for s in per_module.values() if s),
        "references_total": sum(by_symbol.values()),
        "symbols_distinct": len(by_symbol),
        "symbols_some_artefact_defines": len(unlinked),
        "symbols_no_artefact_defines": len(absent),
        "weak_references": sum(len(w) for w in weak_per_module.values()),
        # Reported, never gated: correct by construction, and it moves with how
        # much of the tree builds.
        "sysbase_in_kickstart": expected_refs,
        "library_bases_known": len(libbases),
        # Reported, never gated, for the same reason as the library bases:
        # correct by construction, and moves with how much of the tree builds.
        "load_set_resolved": set_resolved,
        "load_sets": len(load_sets),
        # Reported, never gated: an orphan is an old output name still on disk,
        # which says nothing about the current build. Only worth watching so a
        # rising number prompts a clean.
        "orphan_artefacts": len(orphans),
    }
    # Ratcheted as a fraction, not as a count. The absolute rose from 998 to
    # 1003 on the first real improvement, purely because 16 more modules had
    # started building: 998/1011 became 1003/1027, which is better, and the gate
    # said worse. A count that grows with the number of artefacts cannot be
    # compared across runs.
    measured["modules_with_undefined_permille"] = round(
        1000 * measured["modules_with_undefined"] / measured["artefacts"]
    )

    args.report_dir.mkdir(parents=True, exist_ok=True)

    def write(name: str, lines: list[str]) -> pathlib.Path:
        p = args.report_dir / name
        p.write_text("\n".join(lines) + "\n" if lines else "")
        return p

    write("by-symbol.txt", [
        f"{c:6d}  {'link-missing' if s in provider else 'no-provider '}  {s}"
        for s, c in sorted(by_symbol.items(), key=lambda kv: (-kv[1], kv[0]))
    ])
    write("by-module.txt", [
        f"{len(s):6d}  {m}"
        for m, s in sorted(per_module.items(), key=lambda kv: (-len(kv[1]), kv[0]))
        if s
    ])
    write("expected-sysbase.txt", [
        f"{len(v):6d}  {m}"
        for m, v in sorted(expected.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    ])
    write("orphan-artefacts.txt", [
        str(path.resolve().relative_to(root)) for path in orphans
    ])
    write("no-provider-built.txt", [
        f"{c:6d}  {s}"
        for s, c in sorted(absent.items(), key=lambda kv: (-kv[1], kv[0]))
    ])

    print("🔍 AROS-NG symbol audit")
    for k, v in measured.items():
        print(f"   {k:28s} {v}")
    print(f"   reports                      {args.report_dir}")

    if args.update_baseline:
        if not args.baseline:
            print("symbol audit: --update-baseline needs --baseline", file=sys.stderr)
            return 2
        args.baseline.write_text(json.dumps(measured, indent=2, sort_keys=True) + "\n")
        print(f"   baseline written             {args.baseline}")
        return 0

    if not args.baseline or not args.baseline.is_file():
        print("   no baseline pinned; reporting only")
        return 0

    pinned = json.loads(args.baseline.read_text())
    # Only "fewer is better" metrics ratchet, and only ones that do not move
    # with the number of artefacts. artefacts itself grows as more of the tree
    # builds, so it is reported and not gated, and modules_with_undefined is
    # gated through its permille form for the same reason.
    ratcheted = [
        "modules_with_undefined_permille",
        "references_total",
        "symbols_distinct",
        "symbols_no_artefact_defines",
    ]
    risen = [
        (k, pinned[k], measured[k])
        for k in ratcheted
        if k in pinned and measured[k] > pinned[k]
    ]
    improved = [
        (k, pinned[k], measured[k])
        for k in ratcheted
        if k in pinned and measured[k] < pinned[k]
    ]
    for k, was, now in improved:
        print(f"   ✅ {k}: {was} -> {now}")
    if risen:
        for k, was, now in risen:
            print(f"   ⚠️  {k} rose: {was} -> {now}")
        print("symbol audit: a module lost more symbols than the pinned baseline "
              "allows; see the reports, and re-pin deliberately with "
              "--update-baseline if the rise is intended", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
