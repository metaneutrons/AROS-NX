#!/bin/sh
# Compile the actual ABI10 OTA hook initializer with the real resource header
# across every owner/privacy/update release gate combination. This is a host source
# test, not a hardware or transport qualification.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
source_file="$test_root/network/runtime_probe.c"
mmake_file="$test_root/network/mmakefile.src"
compiler=${CC:-cc}
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-ota-bridge.XXXXXX")
extract="$test_tmp/wifi_ota_hooks_extract.inc"
init_extract="$test_tmp/p4_wifi_init_extract.inc"
init_missing="$test_tmp/p4_wifi_init_missing_call.inc"
counterprobe="$test_tmp/missing_call.out"
default_extract="$test_tmp/c6_update_default.inc"
owner_guard_extract="$test_tmp/c6_update_owner_guard.inc"
owner_guard_out="$test_tmp/c6_update_owner_guard.out"
wrong_gate_dir="$test_tmp/privacy-coupled"
wrong_gate_extract="$wrong_gate_dir/wifi_ota_hooks_extract.inc"
wrong_gate_out="$test_tmp/privacy-coupled.out"
trap 'command rm -f "$test_tmp/test" "$test_tmp/default-off" "$test_tmp/owner-guard-positive" "$test_tmp/owner-guard-negative" "$extract" "$init_extract" "$init_missing" "$counterprobe" "$default_extract" "$test_tmp/c6_update_mmake_gate.inc" "$owner_guard_extract" "$owner_guard_out" "$wrong_gate_out" "$wrong_gate_extract"; if [ -d "$wrong_gate_dir" ]; then command rmdir "$wrong_gate_dir"; fi; command rmdir "$test_tmp"' EXIT HUP INT TERM

# Keep the update-release default, makefile opt-in, and persistent-owner
# assertion tied to their exact production source blocks.
awk '
    $0 == "#ifndef P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED" {
        print
        if (getline <= 0 || $0 != "#define P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED 0") exit 1
        print
        if (getline <= 0 || $0 != "#endif") exit 1
        print
        found = 1
        exit
    }
    END { if (!found) exit 1 }
' "$source_file" > "$default_extract" || {
    echo "C6 update release default is not the exact default-off block" >&2
    exit 1
}
if ! grep -Fqx 'static BOOL wifi_ota_update_enabled = FALSE;' "$source_file"; then
    echo "OTA update release flag is not static and default-off" >&2
    exit 1
fi
printf '%s\n' '#include "c6_update_default.inc"' \
    '#if P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED != 0' \
    '#error "C6 update release default must be zero"' \
    '#endif' 'int main(void) { return 0; }' |
    "$compiler" -std=c99 -Wall -Wextra -Werror -pedantic \
        -I"$test_tmp" -x c - -o "$test_tmp/default-off"

awk '
    $0 == "ifeq ($(P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED),1)" {
        print
        if (getline <= 0 || $0 != "USER_CPPFLAGS += -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED=1") exit 1
        print
        if (getline <= 0 || $0 != "endif") exit 1
        print
        found = 1
        exit
    }
    END { if (!found) exit 1 }
' "$mmake_file" > "$test_tmp/c6_update_mmake_gate.inc" || {
    echo "C6 update make opt-in is not the exact value-1 emission block" >&2
    exit 1
}
if [ "$(grep -Fxc 'USER_CPPFLAGS += -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED=1' "$mmake_file")" -ne 1 ]; then
    echo "C6 update make flag must be emitted exactly once" >&2
    exit 1
fi

awk '
    $0 == "#if P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED && !P4_WIFI_HOSTED_OWNER" {
        print
        if (getline <= 0 || $0 != "#error \"C6 verified update requires the persistent owner\"") exit 1
        print
        if (getline <= 0 || $0 != "#endif") exit 1
        print
        found = 1
        exit
    }
    END { if (!found) exit 1 }
' "$source_file" > "$owner_guard_extract" || {
    echo "C6 update persistent-owner assertion or diagnostic changed" >&2
    exit 1
}
printf '%s\n' '#include "c6_update_owner_guard.inc"' \
    'int main(void) { return 0; }' |
    "$compiler" -std=c99 -Wall -Wextra -Werror -pedantic \
        -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED=1 -DP4_WIFI_HOSTED_OWNER=1 \
        -I"$test_tmp" -x c - -o "$test_tmp/owner-guard-positive"
if printf '%s\n' '#include "c6_update_owner_guard.inc"' \
    'int main(void) { return 0; }' |
    "$compiler" -std=c99 -Wall -Wextra -Werror -pedantic \
        -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED=1 -DP4_WIFI_HOSTED_OWNER=0 \
        -I"$test_tmp" -x c - -o "$test_tmp/owner-guard-negative" \
        > "$owner_guard_out" 2>&1; then
    echo "C6 update persistent-owner compile-time refusal unexpectedly passed" >&2
    exit 1
fi
if ! grep -Fq 'C6 verified update requires the persistent owner' "$owner_guard_out"; then
    echo "C6 update owner guard failed without its expected diagnostic" >&2
    cat "$owner_guard_out" >&2
    exit 1
fi
echo "wifi OTA default-off, make opt-in, and persistent-owner guard verified"

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
    for c6_privacy in 0 1; do
        for host in 0 1; do
            for c6_update in 0 1; do
                for test_opt in 0 2; do
                    "$compiler" -std=c99 -Wall -Wextra -Werror -pedantic \
                        -O"$test_opt" -fsanitize=address,undefined \
                        -fno-omit-frame-pointer \
                        -DP4_WIFI_HOSTED_OWNER="$owner" \
                        -DP4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED="$c6_privacy" \
                        -DP4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED="$host" \
                        -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED="$c6_update" \
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
done

# Negative control: coupling the new update release to the existing C6
# privacy assertion must fail the owner=host=update=1, privacy=0 fixture.
mkdir -p "$wrong_gate_dir"
sed 's/P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED/P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED/g' \
    "$extract" > "$wrong_gate_extract"
if ! grep -Fq 'P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED && P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED' \
    "$wrong_gate_extract"; then
    echo "could not construct privacy-coupled OTA gate counterprobe" >&2
    exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic -O0 \
    -DP4_WIFI_HOSTED_OWNER=1 -DP4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED=0 \
    -DP4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED=1 \
    -DP4_WIFI_C6_UPDATE_RECOVERY_VERIFIED=1 \
    -I"$wrong_gate_dir" -I"$test_tmp" -I"$test_root/network" \
    -isystem "$repo_root/compiler/include" \
    -I"$repo_root/workbench/tools/ESP32Board" \
    "$test_root/tools/test-wifi-ota-bridge.c" -o "$test_tmp/test"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$test_tmp/test" > "$wrong_gate_out" 2>&1; then
    echo "privacy-coupled OTA gate counterprobe unexpectedly passed" >&2
    exit 1
fi
if ! grep -Fq 'wifi_ota_update_enabled == TRUE' "$wrong_gate_out"; then
    echo "privacy-coupled OTA gate counterprobe failed for an unrelated reason" >&2
    cat "$wrong_gate_out" >&2
    exit 1
fi
echo "wifi OTA privacy-coupling mutation rejected"
