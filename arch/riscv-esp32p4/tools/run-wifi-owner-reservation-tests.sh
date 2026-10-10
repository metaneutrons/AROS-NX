#!/bin/sh
# Source-extract the production runtime request/settlement path and link the
# real Hosted owner so the preflight-refusal regression is not exchange-mocked.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-owner-reservation.XXXXXX")
extract="$test_tmp/wifi_owner_reservation_extract.inc"
source_file="$test_root/network/runtime_probe.c"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$extract"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

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

extract_function wifi_owner_finish_unsubmitted_reservation
extract_function wifi_owner_request

for optimization in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-O"$optimization" -I "$test_tmp" -I "$test_root/network" \
		"$test_root/network/hosted_owner.c" \
		"$test_root/network/hosted_ota.c" \
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
		"$test_root/network/hosted_wifi.c" \
		"$test_root/network/hosted_scan.c" \
		"$test_root/network/hosted_association.c" \
		"$test_root/tools/test-wifi-owner-reservation.c" \
		-o "$test_tmp/test-o$optimization"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$optimization"
done
