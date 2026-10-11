#!/bin/sh
# Compile the actual ABI10 OTA hook initializer with the real resource header
# across every owner/privacy/recovery gate combination. This is a host source
# test, not a hardware or transport qualification.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
source_file="$test_root/network/runtime_probe.c"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-ota-bridge.XXXXXX")
extract="$test_tmp/wifi_ota_hooks_extract.inc"
init_extract="$test_tmp/p4_wifi_init_extract.inc"
init_missing="$test_tmp/p4_wifi_init_missing_call.inc"
counterprobe="$test_tmp/missing_call.out"
trap 'command rm -f "$test_tmp/test" "$extract" "$init_extract" "$init_missing" "$counterprobe"; command rmdir "$test_tmp"' EXIT HUP INT TERM

awk -v name=wifi_ota_hooks_init '
    !candidate && $0 ~ "^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(" {
        candidate = 1
        candidate_text = $0 ORS
        next
    }
    candidate {
        candidate_text = candidate_text $0 ORS
        line = $0
        opens = closes = 0
        for (i = 1; i <= length(line); ++i) {
            ch = substr(line, i, 1)
            if (ch == "{") ++opens
            else if (ch == "}") ++closes
        }
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
        opens = closes = 0
        for (i = 1; i <= length(line); ++i) {
            ch = substr(line, i, 1)
            if (ch == "{") ++opens
            else if (ch == "}") ++closes
        }
        depth += opens - closes
        if (depth == 0) { found = 1; exit }
    }
    END { if (!found) exit 1 }
' "$source_file" > "$extract" || {
    echo "could not extract actual wifi_ota_hooks_init" >&2
    exit 1
}

# Registration must call the initializer, not merely define it. Extract the
# actual P4Wifi_Init body so this guard cannot be satisfied by unrelated call
# sites elsewhere in runtime_probe.c.
awk -v name=P4Wifi_Init '
    !candidate && $0 ~ "^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(" {
        candidate = 1
        candidate_text = $0 ORS
        next
    }
    candidate {
        candidate_text = candidate_text $0 ORS
        line = $0
        opens = closes = 0
        for (i = 1; i <= length(line); ++i) {
            ch = substr(line, i, 1)
            if (ch == "{") ++opens
            else if (ch == "}") ++closes
        }
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
        opens = closes = 0
        for (i = 1; i <= length(line); ++i) {
            ch = substr(line, i, 1)
            if (ch == "{") ++opens
            else if (ch == "}") ++closes
        }
        depth += opens - closes
        if (depth == 0) { found = 1; exit }
    }
    END { if (!found) exit 1 }
' "$source_file" > "$init_extract" || {
    echo "could not extract actual P4Wifi_Init" >&2
    exit 1
}

check_init_hook_call() {
    initializer=$1
    call_count=$(awk '
        {
            line = $0
            call = "wifi_ota_hooks_init(P4WifiBase);"
            while ((offset = index(line, call)) != 0) {
                ++count
                line = substr(line, offset + length(call))
            }
        }
        END { print count + 0 }
    ' "$initializer")
    if [ "$call_count" -ne 1 ]; then
        printf 'P4Wifi_Init must call wifi_ota_hooks_init(P4WifiBase) exactly once; found %s\n' \
            "$call_count" >&2
        return 1
    fi
}

check_init_hook_call "$init_extract" || exit 1
sed '/^[[:space:]]*wifi_ota_hooks_init(P4WifiBase);[[:space:]]*$/d' \
    "$init_extract" > "$init_missing"
if grep -Fq 'wifi_ota_hooks_init(P4WifiBase);' "$init_missing"; then
    echo "could not construct missing P4Wifi_Init hook-call counterprobe" >&2
    exit 1
fi
if check_init_hook_call "$init_missing" > "$counterprobe" 2>&1; then
    echo "P4Wifi_Init missing-call counterprobe unexpectedly passed" >&2
    exit 1
fi
if ! grep -Fq \
    'P4Wifi_Init must call wifi_ota_hooks_init(P4WifiBase) exactly once; found 0' \
    "$counterprobe"; then
    echo "P4Wifi_Init missing-call counterprobe failed for an unrelated reason" >&2
    cat "$counterprobe" >&2
    exit 1
fi
echo "wifi OTA P4Wifi_Init hook-call mutation rejected"

for owner in 0 1; do
    for c6 in 0 1; do
        for host in 0 1; do
            for test_opt in 0 2; do
                "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
                    -O"$test_opt" -fsanitize=address,undefined \
                    -fno-omit-frame-pointer \
                    -DP4_WIFI_HOSTED_OWNER="$owner" \
                    -DP4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED="$c6" \
                    -DP4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED="$host" \
                    -I"$test_tmp" -I"$test_root/network" \
                    -isystem "$repo_root/compiler/include" \
                    -I"$repo_root/workbench/tools/ESP32Board" \
                    "$test_root/tools/test-wifi-ota-bridge.c" \
                    -o "$test_tmp/test"
                ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                    "$test_tmp/test"
            done
        done
    done
done
