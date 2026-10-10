#!/bin/sh
# Compile and execute the actual SANA-II frame/TX paths extracted from source.
# Exec lists, user buffers and radio transmission are small explicit mocks.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file="$test_root/network/sana_device.c"
header_file="$test_root/network/sana_device.h"
test_file="$test_root/tools/test-wifi-sana-datapath.c"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-sana-datapath.XXXXXX")
extract="$test_tmp/sana_datapath_extract.inc"
abort_extract="$test_tmp/sana_datapath_abort_extract.inc"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract" "$abort_extract"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"
: > "$abort_extract"

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
        echo "could not extract struct/enum $block_name from $block_file" >&2
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
        echo "could not extract function $function_name from $source_file" >&2
        exit 1
    }
}

extract_block p4wlan_request_kind "$header_file"
extract_block p4wlan_request_state "$header_file"
extract_block p4wlan_request "$header_file"
extract_block p4wlan_opener "$header_file"
extract_block p4_wifi_link_info "$test_root/network/device_bridge.h"
extract_block p4_wifi_association_target "$test_root/network/device_bridge.h"
extract_block p4wlan_association "$header_file"
extract_block p4_wifi_scan_result "$test_root/network/device_bridge.h"
extract_block p4wlan_unit "$header_file"

extract_function p4wlan_copy_bytes
extract_function p4wlan_zero_bytes
extract_function p4wlan_wipe_bytes
extract_function p4wlan_rx_copy
extract_function p4wlan_tx_copy
extract_function p4wlan_find_opener_locked
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
extract_function p4wlan_alloc_request
extract_function p4wlan_enqueue_locked
extract_function p4wlan_validate_write
extract_function p4wlan_frame_callback
extract_function p4wlan_send_one

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
' "$source_file" >> "$abort_extract" || {
    echo "could not extract actual abort_io entry from $source_file" >&2
    exit 1
}

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"

for optimization in 0 2; do
    # Intentional word splitting applies the compiler flags as separate args.
    # shellcheck disable=SC2086
    "$cc" $common_flags "-O$optimization" -I "$test_tmp" "$test_file" \
        -o "$test_tmp/test-o$optimization"
    "$test_tmp/test-o$optimization"
done
