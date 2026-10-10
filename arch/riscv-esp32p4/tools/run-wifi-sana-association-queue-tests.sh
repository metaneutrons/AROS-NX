#!/bin/sh
# Build host tests from the real SANA association queue helpers and selected
# BeginIO switch arms. The harness is a dispatch-arm model, not full BeginIO.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
source_file="$test_root/network/sana_device.c"
header_file="$test_root/network/sana_device.h"
bridge_file="$test_root/network/device_bridge.h"
sana_header="$repo_root/compiler/include/devices/sana2.h"
wireless_header="$repo_root/compiler/include/devices/sana2wireless.h"
io_header="$repo_root/compiler/include/exec/io.h"
errors_header="$repo_root/compiler/include/exec/errors.h"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-sana-association-queue.XXXXXX")
extract="$test_tmp/sana_association_queue_extract.inc"
cases="$test_tmp/sana_association_queue_cases.inc"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract" "$cases"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"
: > "$cases"

UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS ASAN_OPTIONS

extract_define() {
    macro_name=$1
    macro_file=$2
    awk -v name="$macro_name" \
        '$1 == "#define" && $2 == name { print; count++ }
         END { if (count != 1) exit 1 }' "$macro_file" >> "$extract" || {
        echo "could not uniquely extract $macro_name from $macro_file" >&2
        exit 1
    }
}

extract_block() {
    block_kind=$1
    block_name=$2
    block_file=$3
    awk -v name="$block_name" '
        !capture && ($0 ~ "^(struct|enum)[[:space:]]+" name "[[:space:]]*$" ||
                     $0 ~ "^(struct|enum)[[:space:]]+" name "[[:space:]]*\\{") {
            capture = 1
            started = 0
            depth = 0
        }
        capture {
            print
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            if (opens != 0) started = 1
            depth += opens - closes
            if (started && depth == 0) { found = 1; exit }
        }
        END { if (!found) exit 1 }
    ' "$block_file" >> "$extract" || {
        echo "could not extract $block_kind $block_name from $block_file" >&2
        exit 1
    }
}

extract_function() {
    function_name=$1
    matches=$(awk -v name="$function_name" '
        $0 ~ "^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(" { count++ }
        END { print count + 0 }
    ' "$source_file")
    if [ "$matches" -ne 1 ]; then
        echo "expected one source definition of $function_name, found $matches" >&2
        exit 1
    fi
    awk -v name="$function_name" '
        !candidate && !copy && $0 ~ "^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(" {
            candidate = 1
            candidate_text = $0 ORS
            next
        }
        candidate {
            candidate_text = candidate_text $0 ORS
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            if (opens != 0) {
                copy = 1
                depth = opens - closes
                printf "%s", candidate_text
                candidate = 0
                candidate_text = ""
                if (depth == 0) { found = 1; exit }
            } else if (index($0, ";") != 0) {
                candidate = 0
                candidate_text = ""
            }
            next
        }
        copy {
            print
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            depth += opens - closes
            if (depth == 0) { found = 1; exit }
        }
        END { if (!found) exit 1 }
    ' "$source_file" >> "$extract" || {
        echo "could not extract sana_device.c helper: $function_name" >&2
        exit 1
    }
}

extract_case() {
    case_name=$1
    awk -v name="$case_name" '
        $0 ~ "^[[:space:]]*case[[:space:]]+" name ":[[:space:]]*$" {
            count++
            if (count == 1) {
                capture = 1
                print
            } else {
                capture = 0
            }
            next
        }
        capture && $0 ~ "^[[:space:]]*(case[[:space:]]+|default[[:space:]]*:)" {
            capture = 0
            next
        }
        capture { print }
        END { if (count != 1) exit 1 }
    ' "$source_file" >> "$cases" || {
        echo "could not extract BeginIO case $case_name from $source_file" >&2
        exit 1
    }
    printf '\n' >> "$cases"
}

for macro_name in S2EVENT_ONLINE S2EVENT_OFFLINE S2EVENT_CONNECT \
                  S2EVENT_DISCONNECT S2EVENT_CONNECT_FAILED \
                  S2EVENT_ERROR; do
    extract_define "$macro_name" "$sana_header"
done
for macro_name in P4_WIFI_LINK_UNKNOWN P4_WIFI_LINK_UP P4_WIFI_LINK_DOWN; do
    extract_define "$macro_name" "$bridge_file"
done
for macro_name in S2_SETOPTIONS S2_GETNETWORKINFO; do
    extract_define "$macro_name" "$wireless_header"
done
extract_define S2_OFFLINE "$sana_header"
for macro_name in CMD_FLUSH CMD_NONSTD; do
    extract_define "$macro_name" "$io_header"
done
for macro_name in IOERR_ABORTED; do
    extract_define "$macro_name" "$errors_header"
done
for macro_name in S2ERR_NO_RESOURCES S2ERR_BAD_ARGUMENT S2ERR_OUTOFSERVICE \
                  S2WERR_GENERIC_ERROR S2WERR_UNIT_OFFLINE S2WERR_NULL_POINTER; do
    extract_define "$macro_name" "$sana_header"
done

extract_block enum p4wlan_request_kind "$header_file"
extract_block enum p4wlan_request_state "$header_file"
extract_block struct p4wlan_request "$header_file"
extract_block struct p4wlan_opener "$header_file"
extract_block struct p4wlan_association "$header_file"
extract_block struct p4wlan_unit "$header_file"
extract_block struct esp32p4wlan_base "$header_file"
extract_function p4wlan_mac_valid
extract_function p4wlan_mac_equal
extract_function p4wlan_resource_valid
extract_function p4wlan_zero_bytes
extract_function p4wlan_wipe_bytes
extract_function p4wlan_add_done_locked
extract_function p4wlan_first_done
extract_function p4wlan_finish_request
extract_function p4wlan_finish_done
extract_function p4wlan_queue_done_locked
extract_function p4wlan_drop_pin_locked
extract_function p4wlan_retire_association_locked
extract_function p4wlan_invalidate_selection_locked
extract_function p4wlan_cancel_opener_association_locked
extract_function p4wlan_flush_opener_locked
extract_function p4wlan_flush_all_locked
extract_function p4wlan_event_locked
extract_function p4wlan_set_offline_locked
extract_function p4wlan_term_io
extract_function p4wlan_immediate_error
extract_function p4wlan_association_cancelled
extract_function p4wlan_confirmed_target_matches
extract_function p4wlan_association_failed_locked
extract_function p4wlan_associate_one
extract_function p4wlan_link_info_equal
extract_function p4wlan_maintain_link
extract_block struct p4wlan_network_publication "$source_file"
extract_function p4wlan_network_info_locked

for case_name in S2_SETOPTIONS S2_GETNETWORKINFO S2_OFFLINE CMD_FLUSH; do
    extract_case "$case_name"
done

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
        -O"$test_opt" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$test_tmp" -isystem "$repo_root/compiler/include" \
        "$test_root/tools/test-wifi-sana-association-queue.c" \
        -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
