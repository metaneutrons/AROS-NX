#!/bin/sh

set -eu

if [ "$#" -lt 3 ] || [ "$2" != "--" ]; then
    echo "usage: $0 MARKER -- COMMAND [ARG ...]" >&2
    exit 64
fi

marker=$1
shift 2
lockdir="${marker}.lock"

mkdir -p "$(dirname "$marker")"

if [ -f "$marker" ]; then
    exit 0
fi

while ! mkdir "$lockdir" 2>/dev/null; do
    if [ -f "$marker" ]; then
        exit 0
    fi
    sleep 1
done

cleanup()
{
    rmdir "$lockdir" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Another process can finish between the first marker check and our lock.
if [ -f "$marker" ]; then
    exit 0
fi

"$@"
touch "$marker"
