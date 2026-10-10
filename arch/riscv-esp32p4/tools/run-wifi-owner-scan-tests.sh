#!/bin/sh
# Compile the actual owner scan/request functions and runtime structs extracted
# from runtime_probe.c on every run. The exchange/clock/Exec/SDIO boundary is a
# deterministic host fake; this is not hardware or timing acceptance.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-owner-scan.XXXXXX")
extract="$test_tmp/wifi_owner_scan_extract.inc"
source_file="$test_root/network/runtime_probe.c"
header_file="$test_root/network/runtime_probe.h"
bridge_file="$test_root/network/device_bridge.h"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

: > "$extract"

extract_struct() {
	struct_name=$1
	input_file=$2
	awk -v name="$struct_name" '
		!capture && ($0 ~ "^struct[[:space:]]+" name "[[:space:]]*$" ||
		             $0 ~ "^struct[[:space:]]+" name "[[:space:]]*\\{") {
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
	' "$input_file" >> "$extract" || {
		echo "could not extract struct $struct_name from $input_file" >&2
		exit 1
	}
}

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

extract_struct wifi_probe_context "$source_file"
extract_struct p4_wifi_scan_result "$bridge_file"
extract_struct p4_wifi_device_callbacks "$bridge_file"
extract_struct P4WifiBase "$header_file"
extract_struct wifi_owner_control "$source_file"
extract_struct wifi_device_runtime "$source_file"

extract_function wifi_tx_session
extract_function wifi_budget
extract_function wifi_owner_now
extract_function wifi_owner_acquire
extract_function wifi_owner_budget
extract_function wifi_owner_release
extract_function wifi_owner_stopping
extract_function wifi_yield
extract_function wifi_owner_remaining
extract_function wifi_owner_finish_unsubmitted_reservation
extract_function wifi_owner_request
extract_function wifi_scan_output_valid
extract_function wifi_device_scan
extract_function wifi_device_send

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"

for optimization in 0 2; do
	# Intentional word splitting applies compiler flags as separate arguments.
	# shellcheck disable=SC2086
	"$cc" $common_flags "-O$optimization" \
		-I "$test_tmp" -I "$test_root/network" -I "$test_root/sdmmc" \
		-I "$repo_root/compiler/include" \
		"$test_root/network/hosted_scan.c" \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_request_ids.c" \
		"$test_root/network/hosted_wifi.c" \
		"$test_root/network/hosted_mac.c" \
		"$test_root/network/hosted_tx.c" \
		"$test_root/network/hosted_wire.c" \
		"$test_root/network/hosted_fifo.c" \
		"$test_root/network/sdio_commands.c" \
		"$test_root/tools/test-wifi-owner-scan.c" \
		-o "$test_tmp/test-o$optimization"
	"$test_tmp/test-o$optimization"
done
