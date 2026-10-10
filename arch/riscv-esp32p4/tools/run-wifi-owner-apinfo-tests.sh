#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-owner-apinfo.XXXXXX")
source_file="$test_root/network/runtime_probe.c"
extract="$test_tmp/wifi_owner_apinfo_extract.inc"
owner_object="$test_tmp/hosted_owner_noexchange.o"

cleanup()
{
	command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" \
		"$extract" "$owner_object"
	command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"
	rmdir "$test_tmp"
}
trap cleanup EXIT HUP INT TERM

: > "$extract"

# Each expected normalized signature is checked exactly. The extractor counts
# definitions, captures one complete brace-balanced body, and rejects changed
# signatures, duplicate definitions, prototypes in place of definitions, or
# missing helpers.
extract_function()
{
	function_name=$1
	expected_signature=$2
	if ! awk -v name="$function_name" -v expected="$expected_signature" '
	function normalize(text) {
		gsub(/[[:space:]]+/, " ", text)
		sub(/^ /, "", text)
		sub(/ $/, "", text)
		gsub(/\( /, "(", text)
		gsub(/ \)/, ")", text)
		return text
	}
	function brace_delta(text, copy, opens, closes) {
		copy = text
		opens = gsub(/\{/, "", copy)
		closes = gsub(/\}/, "", copy)
		return opens - closes
	}
	!candidate && $0 ~ ("^[[:space:]]*static[[:space:]].*" name "[[:space:]]*\\(") {
		candidate = 1
		candidate_text = $0 ORS
		signature_text = $0 " "
		if (index($0, ";") != 0 && index($0, "{") == 0) {
			candidate = 0
			candidate_text = ""
			signature_text = ""
		}
		next
	}
	candidate {
		candidate_text = candidate_text $0 ORS
		signature_text = signature_text $0 " "
		if (index($0, ";") != 0 && index($0, "{") == 0) {
			candidate = 0
			candidate_text = ""
			signature_text = ""
			next
		}
		if (index($0, "{") != 0) {
			sig = signature_text
			sub(/\{.*/, "", sig)
			definitions++
			if (definitions == 1) {
				captured = candidate_text
				if (normalize(sig) != expected) bad = 1
				copying = 1
				depth = brace_delta($0)
				if (depth == 0) {
					found = 1
					copying = 0
				}
			} else {
				bad = 1
			}
			candidate = 0
			candidate_text = ""
			signature_text = ""
			next
		}
		next
	}
	copying {
		captured = captured $0 ORS
		depth += brace_delta($0)
		if (depth == 0) {
			found = 1
			copying = 0
		}
	}
	END {
		if (definitions != 1 || !found || bad) exit 1
		printf "%s", captured
	}
	' "$source_file" >> "$extract"; then
		echo "could not uniquely extract expected runtime_probe.c function: $function_name" >&2
		exit 1
	fi
}

extract_function wifi_owner_event \
	'static int wifi_owner_event(void *opaque, const struct p4_hosted_inbox_item *item)'
extract_function wifi_owner_now \
	'static uint32_t wifi_owner_now(void *opaque)'
extract_function wifi_tx_session \
	'static int wifi_tx_session(void *opaque, uint32_t generation)'
extract_function wifi_owner_acquire \
	'static int wifi_owner_acquire(void *opaque, uint32_t wait_us)'
extract_function wifi_owner_release \
	'static void wifi_owner_release(void *opaque)'
extract_function wifi_owner_finish_unsubmitted_reservation \
	'static int wifi_owner_finish_unsubmitted_reservation(struct p4_hosted_owner *owner, struct p4_hosted_fifo *fifo, uint32_t generation, uint32_t uid, uint32_t request_id, unsigned submitted)'
extract_function wifi_owner_request \
	'static int wifi_owner_request(struct wifi_probe_context *context, struct p4_hosted_owner *owner, struct wifi_owner_control *control, struct p4_hosted_fifo *fifo, unsigned checksum, uint32_t id, ULONG remaining)'
extract_function wifi_owner_remaining \
	'static ULONG wifi_owner_remaining(struct wifi_probe_context *context, ULONG started, ULONG limit)'
extract_function wifi_owner_refresh_link_snapshot \
	'static int wifi_owner_refresh_link_snapshot(struct wifi_probe_context *context, struct p4_hosted_owner *owner, struct wifi_owner_control *control, struct p4_hosted_fifo *fifo, unsigned checksum)'

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"

for optimization in 0 2; do
	# The real owner implementation is linked for retirement/cancel checks, but
	# its exchange symbol is renamed so the test supplies a bounded fake exchange.
	# Intentional splitting applies the fixed flags as separate compiler args.
	# shellcheck disable=SC2086
	"$cc" $common_flags "-O$optimization" -I "$test_root/network" \
		-Dp4_hosted_owner_exchange=p4_hosted_owner_exchange_unused \
		-c "$test_root/network/hosted_owner.c" -o "$owner_object"

	# shellcheck disable=SC2086
	"$cc" $common_flags "-O$optimization" -I "$test_tmp" \
		-I "$test_root/network" \
		"$test_root/network/hosted_pending.c" \
		"$test_root/network/hosted_ota.c" \
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
		"$test_root/network/hosted_link.c" \
		"$test_root/tools/test-wifi-owner-apinfo.c" \
		"$owner_object" -o "$test_tmp/test-o$optimization"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$optimization"
done
