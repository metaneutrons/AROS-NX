#!/bin/sh
# Compile and run the actual extracted SANA-II scan helpers. The radio,
# semaphore, request completion, and pool are deterministic host fakes; this
# does not qualify hardware or Exec scheduling.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file="$test_root/network/sana_device.c"
header_file="$test_root/network/sana_device.h"
bridge_file="$test_root/network/device_bridge.h"
test_file="$test_root/tools/test-wifi-sana-scan.c"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-sana-scan.XXXXXX")
extract="$test_tmp/sana_scan_extract.inc"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

awk '$1 == "#define" && $2 == "P4WLAN_TAG_LIMIT" { print; found = 1 }
     END { if (!found) exit 1 }' "$source_file" >> "$extract" || {
    echo "could not extract P4WLAN_TAG_LIMIT from $source_file" >&2
    exit 1
}

extract_block() {
    block_name=$1
    block_file=$2
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
            if (started && depth == 0) {
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$block_file" >> "$extract" || {
        echo "could not extract $block_name from $block_file" >&2
        exit 1
    }
}

extract_function() {
    function_name=$1
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
                if (depth == 0) {
                    found = 1
                    exit
                }
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
            if (depth == 0) {
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$source_file" >> "$extract" || {
        echo "could not extract sana_device.c function: $function_name" >&2
        exit 1
    }
}

# Compile the real request/unit and bridge result layouts, along with the
# actual scan helper functions from the current source snapshot.
extract_block p4wlan_request_kind "$header_file"
extract_block p4wlan_request_state "$header_file"
extract_block p4wlan_opener "$header_file"
extract_block p4wlan_request "$header_file"
extract_block p4_wifi_link_info "$bridge_file"
extract_block p4_wifi_scan_result "$bridge_file"
extract_block p4wlan_unit "$header_file"
extract_block p4wlan_scan_public_record "$source_file"
extract_block p4wlan_scan_publication "$source_file"

extract_function p4wlan_copy_bytes
extract_function p4wlan_zero_bytes
extract_function p4wlan_wipe_bytes
extract_function p4wlan_scan_record_matches
extract_function p4wlan_scan_one
extract_function p4wlan_scan_copy_filter

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"
# A sanitizer diagnostic must fail qualification, even if the runtime's
# default recovery mode would otherwise let the assertions finish.
UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS ASAN_OPTIONS

for optimization in 0 2; do
    # Intentional word splitting passes the compiler flags as separate args.
    # shellcheck disable=SC2086
    "$cc" $common_flags "-O$optimization" -I "$test_tmp" \
        -I "$test_root/../../compiler/include" "$test_file" \
        -o "$test_tmp/test-o$optimization"
    "$test_tmp/test-o$optimization"
done
