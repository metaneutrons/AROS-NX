#!/bin/sh
# Exercise the actual resource bridge statements with host-only Exec/SDIO
# mocks. Bridge functions and the stop-publication block are extracted from
# runtime_probe.c on every run, so the test cannot silently use a copied fork.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-device-bridge.XXXXXX")
extract="$test_tmp/wifi_device_bridge_extract.inc"
source_file="$test_root/network/runtime_probe.c"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

extract_function() {
    function_name=$1
    awk -v name="$function_name" '
        !candidate && $0 ~ "^[[:space:]]*(static[[:space:]]+)?[A-Za-z_][A-Za-z0-9_ *]*[[:space:]]+" name "[[:space:]]*\\(" {
            candidate = 1
            candidate_text = $0 ORS
            if (index($0, ";") != 0) {
                candidate = 0
                candidate_text = ""
            }
            next
        }
        candidate {
            candidate_text = candidate_text $0 ORS
            line = $0
            opens = gsub(/\{/, "", line)
            closes = gsub(/\}/, "", line)
            if (opens != 0) {
                copy = 1
                started = 1
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
        echo "could not extract runtime_probe.c function: $function_name" >&2
        exit 1
    }
}

# Runtime-owned data structures are also source-extracted so added bridge
# state, such as the servicing gate, is part of the compiled test layout.
awk '
    /^struct wifi_owner_control \{/ || /^struct wifi_device_runtime \{/ {
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
        if (started && depth == 0) capture = 0
    }
    /^struct wifi_device_runtime \{/ { saw_runtime = 1 }
    END { if (!saw_runtime) exit 1 }
' "$source_file" >> "$extract" || {
    echo "could not extract bridge runtime structures" >&2
    exit 1
}

extract_function wifi_device_attach
extract_function wifi_device_detach
extract_function wifi_tx_session
extract_function wifi_owner_acquire
extract_function wifi_owner_remaining
extract_function wifi_owner_budget
extract_function wifi_owner_release
extract_function wifi_device_send
extract_function wifi_owner_frame

# Compile the exact owner-stop publication statements as a small callable
# block. The preceding ObtainSemaphore and following ReleaseSemaphore are
# included from the source, so the test observes callback ordering and lock.
awk '
    BEGIN { print "static void wifi_test_owner_stop_publish(struct P4WifiBase *base) {" }
    /^static void wifi_owner_service\(/ { in_service = 1 }
    in_service && !capture && $0 ~ /base->ready = FALSE;/ {
        if (previous !~ /ObtainSemaphore\(&base->client_lock\);/) exit 2
        print previous
        print
        capture = 1
        found = 1
        next
    }
    capture {
        print
        if ($0 ~ /ReleaseSemaphore\(&base->client_lock\);/) {
            print "}"
            closed = 1
            exit
        }
    }
    { previous = $0 }
    END { if (!found || !closed) exit 1 }
' "$source_file" >> "$extract" || {
    echo "could not extract owner stop-publication block" >&2
    exit 1
}

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
        -O"$test_opt" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$test_tmp" -I"$test_root/network" -I"$test_root/sdmmc" \
        -I"$repo_root/compiler/include" \
        "$test_root/network/hosted_owner.c" \
        "$test_root/network/hosted_pending.c" \
        "$test_root/network/hosted_rx.c" \
        "$test_root/network/hosted_inbox.c" \
        "$test_root/network/hosted_frames.c" \
        "$test_root/network/hosted_control.c" \
        "$test_root/network/hosted_request_ids.c" \
        "$test_root/network/hosted_tx.c" \
        "$test_root/network/hosted_fifo.c" \
        "$test_root/network/sdio_commands.c" \
        "$test_root/network/hosted_mac.c" \
        "$test_root/network/hosted_epoch_reassembly.c" \
        "$test_root/network/hosted_rpc.c" \
        "$test_root/network/hosted_serial.c" \
        "$test_root/network/hosted_wire.c" \
        "$test_root/network/hosted_init.c" \
        "$test_root/tools/test-wifi-device-bridge.c" \
        -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
