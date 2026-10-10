#!/bin/sh
# Extract and exercise the production SANA-II event/configuration helpers.
# The unit/opener/IO structs are source-derived; only Exec list primitives and
# the unrelated offline flush are deterministic host fakes.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
source_file="$test_root/network/sana_device.c"
header_file="$test_root/network/sana_device.h"
bridge_file="$test_root/network/device_bridge.h"
sana_header="$repo_root/compiler/include/devices/sana2.h"
test_file="$test_root/tools/test-wifi-sana-events.c"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-sana-events.XXXXXX")
extract="$test_tmp/sana_events_extract.inc"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

require_one_block() {
    file=$1
    name=$2
    description=$3
    matches=$(awk -v name="$name" \
        '($1 == "struct" || $1 == "enum") && $2 == name &&
         ($3 == "" || $3 == "{") { count++ }
         END { print count + 0 }' "$file")
    if [ "$matches" -ne 1 ]; then
        echo "expected one $description in $file, found $matches" >&2
        exit 1
    fi
}

require_one_function() {
    file=$1
    name=$2
    description=$3
    matches=$(awk -v name="$name" \
        '/^[[:space:]]*static[[:space:]]/ && index($0, name "(") { count++ }
         END { print count + 0 }' "$file")
    if [ "$matches" -ne 1 ]; then
        echo "expected one $description in $file, found $matches" >&2
        exit 1
    fi
}

extract_constant() {
    name=$1
    definition_file=$2
    awk -v name="$name" '$1 == "#define" && $2 == name { print; count++ }
        END { if (count != 1) exit 1 }' "$definition_file" >> "$extract" || {
        echo "could not uniquely extract $name from $definition_file" >&2
        exit 1
    }
}

extract_block() {
    block_name=$1
    block_file=$2
    require_one_block "$block_file" "$block_name" "$block_name definition"
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
    require_one_function "$source_file" "$function_name" "$function_name definition"
    awk -v name="$function_name" '
        !candidate && !copy && $0 ~ /^[[:space:]]*static[[:space:]]/ &&
            index($0, name "(") != 0 {
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
        echo "could not extract $function_name from $source_file" >&2
        exit 1
    }
}

# Keep the S2_ONEVENT and CONFIGINTERFACE switch arms connected to the tested
# helpers; source extraction alone should not silently test dead code.
awk '
    /^    case S2_ONEVENT:/ { one_count++; in_one = 1; next }
    in_one && /^    case / { in_one = 0 }
    in_one && /p4wlan_onevent_immediate_locked\(unit, request, event_mask\)/ { one_helper = 1 }
    in_one && /p4wlan_alloc_request\(opener, request,/ { one_queue = 1 }
    /^    case S2_CONFIGINTERFACE:/ { config_count++; in_config = 1; next }
    in_config && /^    case / { in_config = 0 }
    in_config && /p4wlan_configure_interface_locked\(unit, &done\)/ { config_helper = 1 }
    in_config && /S2EVENT_(CONNECT|DISCONNECT)/ { config_fabricates_link = 1 }
    /^    case S2_ONLINE:/ { online_count++; in_online = 1; next }
    in_online && /^    case / { in_online = 0 }
    in_online && /p4wlan_online_locked\(unit, &done\)/ { online_helper = 1 }
    in_online && /S2EVENT_(CONNECT|DISCONNECT)/ { online_fabricates_link = 1 }
    /^    case S2_OFFLINE:/ { offline_count++; in_offline = 1; next }
    in_offline && /^    case / { in_offline = 0 }
    in_offline && /p4wlan_set_offline_locked\(unit, &done,/ { offline_helper = 1 }
    END {
        if (one_count != 1 || !one_helper || !one_queue ||
            config_count != 1 || !config_helper || config_fabricates_link ||
            online_count != 1 || !online_helper || online_fabricates_link ||
            offline_count != 1 || !offline_helper) exit 1
    }
' "$source_file" || {
    echo "SANA event/configuration dispatch no longer uses tested helpers" >&2
    exit 1
}

awk '
    /^static BOOL GM_UNIQUENAME\(Init\)\(/ { in_init = 1 }
    in_init && /^}/ { in_init = 0 }
    in_init && /unit->pending_events = 0;/ { reset = 1 }
    END { if (!reset) exit 1 }
' "$source_file" || {
    echo "device Init must explicitly clear its unit-local pending event latch" >&2
    exit 1
}

for constant in S2ERR_NOT_SUPPORTED S2ERR_OUTOFSERVICE S2WERR_BAD_EVENT \
                S2WERR_UNIT_OFFLINE S2EVENT_ONLINE S2EVENT_OFFLINE \
                S2EVENT_CONNECT S2EVENT_DISCONNECT; do
    extract_constant "$constant" "$sana_header"
done
extract_constant P4WLAN_MAX_FRAME "$header_file"

extract_block p4wlan_request_kind "$header_file"
extract_block p4wlan_request_state "$header_file"
extract_block p4wlan_request "$header_file"
extract_block p4wlan_opener "$header_file"
extract_block p4_wifi_link_info "$bridge_file"
extract_block p4_wifi_association_target "$bridge_file"
extract_block p4wlan_association "$header_file"
extract_block p4_wifi_scan_result "$bridge_file"
extract_block p4wlan_unit "$header_file"

extract_function p4wlan_event_locked
extract_function p4wlan_event_mask_valid
extract_function p4wlan_event_ready_locked
extract_function p4wlan_onevent_immediate_locked
extract_function p4wlan_wipe_bytes
extract_function p4wlan_drop_pin_locked
extract_function p4wlan_retire_association_locked
extract_function p4wlan_invalidate_selection_locked
extract_function p4wlan_online_locked
extract_function p4wlan_configure_interface_locked
extract_function p4wlan_set_offline_locked

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"
UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS ASAN_OPTIONS

for optimization in 0 2; do
    # Intentional word splitting passes compiler flags as separate arguments.
    # shellcheck disable=SC2086
    "$cc" $common_flags "-O$optimization" -I "$test_tmp" \
        "$test_file" -o "$test_tmp/test-o$optimization"
    "$test_tmp/test-o$optimization"
done
