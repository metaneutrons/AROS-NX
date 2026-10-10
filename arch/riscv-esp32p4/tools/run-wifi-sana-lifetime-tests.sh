#!/bin/sh
# Build and run source-extracted SANA-II request-lifetime helpers with host
# Exec/MinList mocks. The helper bodies and ABI structs are extracted from the
# production source/header on each run; this is not a parallel implementation.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
source_file="$test_root/network/sana_device.c"
header_file="$test_root/network/sana_device.h"
bridge_file="$test_root/network/device_bridge.h"
sana_header="$repo_root/compiler/include/devices/sana2.h"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-sana-lifetime.XXXXXX")
extract="$test_tmp/sana_lifetime_extract.inc"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

for event_name in S2EVENT_CONNECT S2EVENT_DISCONNECT; do
    awk -v name="$event_name" \
        '$1 == "#define" && $2 == name { print; count++ }
         END { if (count != 1) exit 1 }' "$sana_header" >> "$extract" || {
        echo "could not uniquely extract $event_name from $sana_header" >&2
        exit 1
    }
done

extract_block() {
    block_kind=$1
    block_name=$2
    block_file=$3
    awk -v kind="$block_kind" -v name="$block_name" '
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
        echo "could not extract $block_kind $block_name from $block_file" >&2
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
        echo "could not extract sana_device.c helper: $function_name" >&2
        exit 1
    }
}

extract_abort_entry() {
    awk '
        !capture && /^AROS_LH1\(LONG, abort_io,/ { capture = 1 }
        capture {
            print
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            depth += opens - closes
            if (opens != 0) started = 1
            if (started && depth == 0) {
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$source_file" >> "$extract" || {
        echo "could not extract the actual abort_io entry from sana_device.c" >&2
        exit 1
    }
}

extract_close_entry() {
    awk '
        !capture && /^static BOOL GM_UNIQUENAME\(Close\)/ { capture = 1 }
        capture {
            print
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            depth += opens - closes
            if (opens != 0) started = 1
            if (started && depth == 0) {
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$source_file" >> "$extract" || {
        echo "could not extract the actual Close body from sana_device.c" >&2
        exit 1
    }
}

# Compile the state layout directly from the ABI header, including enum values.
extract_block enum p4wlan_request_kind "$header_file"
extract_block enum p4wlan_request_state "$header_file"
extract_block struct p4wlan_request "$header_file"
extract_block struct p4wlan_opener "$header_file"
extract_block struct p4_wifi_link_info "$bridge_file"
extract_block struct p4_wifi_association_target "$bridge_file"
extract_block struct p4wlan_association "$header_file"
extract_block struct p4_wifi_scan_result "$test_root/network/device_bridge.h"
extract_block struct p4wlan_unit "$header_file"
extract_block struct esp32p4wlan_base "$header_file"

# Extract the lifecycle helpers exercised by the production AbortIO and
# completion paths, followed by the actual AROS entry body.
extract_function p4wlan_find_opener_locked
extract_function p4wlan_add_done_locked
extract_function p4wlan_first_done
extract_function p4wlan_finish_request
extract_function p4wlan_finish_done
extract_function p4wlan_queue_done_locked
extract_function p4wlan_wipe_bytes
extract_function p4wlan_drop_pin_locked
extract_function p4wlan_retire_association_locked
extract_function p4wlan_invalidate_selection_locked
extract_function p4wlan_cancel_opener_association_locked
extract_function p4wlan_flush_opener_locked
extract_function p4wlan_flush_all_locked
extract_function p4wlan_event_locked
extract_function p4wlan_alloc_request
extract_function p4wlan_enqueue_locked
extract_abort_entry
extract_close_entry

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
        -O"$test_opt" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$test_tmp" "$test_root/tools/test-wifi-sana-lifetime.c" \
        -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
