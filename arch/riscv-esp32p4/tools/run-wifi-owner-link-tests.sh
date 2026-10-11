#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-owner-link.XXXXXX")
source_file="$test_root/network/runtime_probe.c"
bridge_file="$test_root/network/device_bridge.h"
extract="$test_tmp/wifi_owner_event_extract.inc"
types="$test_tmp/wifi_owner_link_types.inc"

cleanup()
{
	command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract" "$types"
	command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"
	rmdir "$test_tmp"
}
trap cleanup EXIT HUP INT TERM

# Fail closed if the actual callback is missing, duplicated, or its parameter
# signature changes. Capture one complete brace-balanced definition only.
if ! awk '
function brace_delta(text, copy, opens, closes) {
	copy = text
	opens = gsub(/\{/, "", copy)
	closes = gsub(/\}/, "", copy)
	return opens - closes
}
$0 == "static int wifi_owner_event(void *opaque," {
	signatures++
	if (signatures != 1) next
	captured = $0 ORS
	if (getline <= 0 ||
	    $0 !~ /^[[:space:]]+const struct p4_hosted_inbox_item \*item\)[[:space:]]*$/) {
		bad = 1
		next
	}
	captured = captured $0 ORS
	started = 0
	depth = 0
	while (getline > 0) {
		captured = captured $0 ORS
		delta = brace_delta($0)
		if (delta != 0 || index($0, "{") != 0) started = 1
		depth += delta
		if (started && depth == 0) {
			found = 1
			break
		}
	}
	if (!found) bad = 1
}
END {
	if (signatures != 1 || !found || bad) exit 1
	printf "%s", captured
}
' "$source_file" > "$extract"; then
	echo "could not uniquely extract expected wifi_owner_event signature from $source_file" >&2
	exit 1
fi

# Use the actual ABI6 link-info declaration and link-state constants. Keep the
# owner-event fixture narrow without duplicating those public bridge layouts.
if ! awk '
    /^#define P4_WIFI_LINK_(UNKNOWN|UP|DOWN)[[:space:]]/ {
        name = $2
        counts[name]++
        lines[name] = lines[name] $0 ORS
    }
    END {
        names[1] = "P4_WIFI_LINK_UNKNOWN"
        names[2] = "P4_WIFI_LINK_UP"
        names[3] = "P4_WIFI_LINK_DOWN"
        for (i = 1; i <= 3; ++i) {
            if (counts[names[i]] != 1) exit 1
            printf "%s", lines[names[i]]
        }
    }
' "$bridge_file" > "$types"; then
	echo "could not uniquely extract ABI6 link-state constants from $bridge_file" >&2
	exit 1
fi
if ! awk '
    !capture && $0 ~ /^struct[[:space:]]+p4_wifi_link_info[[:space:]]*\{/ {
        signatures++
        if (signatures != 1) next
        capture = 1
        started = 0
        depth = 0
    }
    capture {
        captured = captured $0 ORS
        line = $0
        opens = gsub(/\{/, "", line)
        closes = gsub(/\}/, "", line)
        if (opens != 0) started = 1
        depth += opens - closes
        if (started && depth == 0) { found = 1; capture = 0 }
    }
    END {
        if (signatures != 1 || !found) exit 1
        printf "%s", captured
    }
' "$bridge_file" >> "$types"; then
	echo "could not uniquely extract ABI6 p4_wifi_link_info from $bridge_file" >&2
	exit 1
fi

for optimization in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -g \
		-O"$optimization" -fsanitize=address,undefined \
		-fno-omit-frame-pointer -I "$test_tmp" \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_association.c" \
		"$test_root/network/hosted_events.c" \
		"$test_root/network/hosted_link.c" \
		"$test_root/tools/test-wifi-owner-link.c" \
		-o "$test_tmp/test-o$optimization"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$optimization"
done
