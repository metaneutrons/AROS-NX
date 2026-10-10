#!/bin/sh
# Source-extract production maintenance/discovery helpers. The fixture models
# Exec, codecs, UID reservation, private owner exchange and host ownership.
# Separate suites cover actual codecs/owner transport; no hardware behavior is claimed.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-maintenance.XXXXXX")
extract="$test_tmp/wifi_maintenance_extract.inc"
mutant_extract="$test_tmp/wifi_maintenance_mutant.inc"
constants="$test_tmp/wifi_maintenance_constants.inc"
source_file="$test_root/network/runtime_probe.c"
header_file="$test_root/network/device_bridge.h"
discovery_header="$test_root/network/hosted_discovery.h"
rpc_header="$test_root/network/hosted_rpc.h"
owner_header="$test_root/network/hosted_owner.h"
layout_header="$test_root/../../workbench/tools/ESP32Board/layout.h"
wire_header="$test_root/boardtool/discovery_wire.h"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/test-mutant" "$test_tmp/mutant.out" "$extract" "$mutant_extract" "$constants"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM" "$test_tmp/test-mutant.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

extract_function() {
	function_name=$1
	awk -v name="$function_name" '
		!candidate && $0 ~ "^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(" {
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
		echo "could not extract runtime_probe.c function: $function_name" >&2
		exit 1
	}
}

# Exercise the production helper too, so clock wrap and expiry use the exact
# subtraction semantics compiled into runtime_probe.c.
extract_function wifi_owner_remaining
extract_function wifi_maintenance_normal_poll
extract_function wifi_maintenance_discovery_clear
extract_function wifi_discovery_output_valid
extract_function wifi_discovery_session_valid
extract_function wifi_maintenance_request
extract_function wifi_maintenance_poll
extract_function wifi_maintenance_release
extract_function wifi_maintenance_discover
extract_function wifi_maintenance_discovery_poll
extract_function wifi_maintenance_discovery_service
extract_function wifi_maintenance_service

# Keep the source wiring that gates the owner FIFO poll under regression
# coverage. An unconditional owner_step call must fail this guard.
awk '
	index($0, "poll_transport = wifi_maintenance_normal_poll(base->maintenance_state);") {
		assignments++
	}
	index($0, "p4_hosted_owner_step(") {
		calls++
		if (index($0, "if (poll_transport && !p4_hosted_owner_step(") != 0)
			guarded++
	}
	END { if (assignments != 1 || calls != 1 || guarded != 1) exit 1 }
' "$source_file" || {
	echo "owner transport polling is not gated by wifi_maintenance_normal_poll" >&2
	exit 1
}

# The source-extracted clear helper is exercised by the C fixture. Confirm the
# actual owner teardown invokes it before dropping the runtime pointer.
awk '
	index($0, "base->ready = FALSE;") { teardown = 1; next }
	teardown && index($0, "wifi_maintenance_discovery_clear(base->runtime, P4_WIFI_DISCOVERY_FAILED);") { clear = 1 }
	teardown && index($0, "base->runtime = NULL;") { runtime_clear = 1; exit }
	END { if (!teardown || !clear || !runtime_clear) exit 1 }
' "$source_file" || {
	echo "owner teardown does not clear discovery before retiring runtime" >&2
	exit 1
}

# These public ABI values come from the production header, not a test copy.
awk '$1 == "#define" && $2 ~ /^P4_WIFI_(MAINT|LINK|DISCOVERY)_/ { print }' \
	"$header_file" > "$constants"
awk '$1 == "#define" && $2 == "P4_HOSTED_DISCOVERY_REQUEST_ID" { print }' \
	"$discovery_header" >> "$constants"
awk '$1 == "#define" && $2 == "P4_HOSTED_RPC_MAX_SIZE" { print }' \
	"$rpc_header" >> "$constants"
awk '$1 == "#define" && $2 == "P4_HOSTED_OWNER_MAX_ACQUIRE_US" { print }' \
	"$owner_header" >> "$constants"
awk '$1 == "#define" && $2 == "BOARD_LAYOUT_MAX_PARTITIONS" { print }' \
	"$layout_header" >> "$constants"
awk '$1 == "#define" && $2 == "BOARD_DISCOVERY_WIRE_MAX" { print }' \
	"$wire_header" >> "$constants"

for optimization in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-O"$optimization" -I "$test_tmp" \
		"$test_root/tools/test-wifi-maintenance.c" \
		-o "$test_tmp/test-o$optimization"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$optimization"
done

# Negative control: inject ACTIVE into a temporary extraction and prove the
# status matrix rejects that unsafe widening. Production source stays untouched.
sed 's/state == P4_WIFI_MAINT_REQUESTED;/state == P4_WIFI_MAINT_REQUESTED || state == P4_WIFI_MAINT_ACTIVE;/' \
	"$extract" > "$mutant_extract"
if ! grep -Fq 'state == P4_WIFI_MAINT_REQUESTED || state == P4_WIFI_MAINT_ACTIVE;' \
	"$mutant_extract"; then
	echo "could not construct ACTIVE-allowed maintenance poll mutation" >&2
	exit 1
fi
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_MAINTENANCE_MUTANT -I "$test_tmp" \
	"$test_root/tools/test-wifi-maintenance.c" -o "$test_tmp/test-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-mutant" > "$test_tmp/mutant.out" 2>&1; then
	echo "ACTIVE-allowed maintenance poll mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq 'wifi_maintenance_normal_poll(P4_WIFI_MAINT_ACTIVE) == FALSE' \
	"$test_tmp/mutant.out"; then
	echo "mutation failed for a reason other than the ACTIVE poll assertion" >&2
	cat "$test_tmp/mutant.out" >&2
	exit 1
fi
echo "wifi maintenance ACTIVE-poll mutation rejected"
