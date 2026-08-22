#!/bin/sh
#
#   Verify a generated FAT32 test image with tools that did not build it.
#
#   Three things are checked, in the order that matters:
#
#     1. the host's own MBR and FAT32 parser accepts the image,
#     2. fsck reports a clean file system,
#     3. the mounted contents match the generated manifest exactly.
#
#   The mount is done on a COPY, never on the image itself.  macOS writes to
#   a FAT volume as soon as it mounts it: it creates .fseventsd, which changes
#   the image and would invalidate the before/after hash the board runs rely
#   on.  The original is hashed before and after to prove it was untouched.
#
#   No root privileges are needed for any of this.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 IMAGE" >&2
    exit 2
fi

img=$1
base=$(dirname "$img")/$(basename "$img" .img)
manifest="$base.manifest"

[ -f "$img" ] || { echo "no such image: $img" >&2; exit 1; }
[ -f "$manifest" ] || { echo "no manifest: $manifest" >&2; exit 1; }

before=$(shasum -a 256 "$img" | cut -d' ' -f1)
echo "image sha256 before: $before"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$img" "$work/copy.img"

echo "--- host parser ---"
attach=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount \
         "$work/copy.img")
printf '%s\n' "$attach"
whole=$(printf '%s\n' "$attach" | awk '/FDisk_partition_scheme/{print $1}' | head -1)
slice=$(printf '%s\n' "$attach" | awk '/DOS_FAT_32/{print $1}' | head -1)
[ -n "$slice" ] || { hdiutil detach "$whole" >/dev/null; \
                     echo "host did not recognise a FAT32 partition" >&2; exit 1; }

echo "--- fsck ---"
fsck_msdos -n "$slice" || { hdiutil detach "$whole" >/dev/null; \
                            echo "fsck rejected the file system" >&2; exit 1; }
hdiutil detach "$whole" >/dev/null

echo "--- manifest against mounted contents ---"
attach=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$work/copy.img")
whole=$(printf '%s\n' "$attach" | awk '/FDisk_partition_scheme/{print $1}' | head -1)
mp=$(printf '%s\n' "$attach" | awk -F'\t' '/Volumes/{print $NF}' | head -1)
[ -n "$mp" ] || { hdiutil detach "$whole" >/dev/null; \
                  echo "image did not mount" >&2; exit 1; }

status=0
python3 "$(dirname "$0")/check-manifest.py" "$mp" "$manifest" || status=$?
hdiutil detach "$whole" >/dev/null

after=$(shasum -a 256 "$img" | cut -d' ' -f1)
echo "image sha256 after:  $after"
if [ "$before" != "$after" ]; then
    echo "THE IMAGE CHANGED DURING VERIFICATION" >&2
    exit 1
fi
echo "original image unchanged"
exit $status
