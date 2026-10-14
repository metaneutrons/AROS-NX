#!/bin/sh
# Compile the production association workflow and helpers extracted verbatim
# from runtime_probe.c. The RPC exchange/host/clock boundary is a host fake.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-owner-association.XXXXXX")
extract="$test_tmp/wifi_owner_association_extract.inc"
source_file="$test_root/network/runtime_probe.c"
header_file="$test_root/network/runtime_probe.h"
bridge_file="$test_root/network/device_bridge.h"
owner_object="$test_tmp/hosted_owner_noexchange.o"
cleanup()
{
	command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" \
		"$extract" "$owner_object"
	command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"
	command rmdir "$test_tmp"
}
trap cleanup EXIT HUP INT TERM

: > "$extract"

extract_struct()
{
	struct_name=$1
	input_file=$2
	if ! awk -v name="$struct_name" '
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
			if (opens) started = 1
			depth += opens - closes
			if (started && depth == 0) { found = 1; exit }
		}
		END { if (!found) exit 1 }
	' "$input_file" >> "$extract"; then
		echo "could not extract struct $struct_name from $input_file" >&2
		exit 1
	fi
}

extract_function()
{
	function_name=$1
	expected_signature=$2
	allow_alternative=${3:-no}
	if ! awk -v name="$function_name" -v expected="$expected_signature" \
		-v alternatives="$allow_alternative" '
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
			candidate = 0; candidate_text = ""; signature_text = ""
		}
		next
	}
	candidate {
		candidate_text = candidate_text $0 ORS
		signature_text = signature_text $0 " "
		if (index($0, ";") != 0 && index($0, "{") == 0) {
			candidate = 0; candidate_text = ""; signature_text = ""; next
		}
		if (index($0, "{") != 0) {
			sig = signature_text
			sub(/\{.*/, "", sig)
			definitions++
			if ((definitions != 1 && alternatives != "yes") ||
			    normalize(sig) != expected) bad = 1
			if (definitions == 1) {
				captured = candidate_text
				copying = 1
				depth = brace_delta($0)
				if (depth == 0) { found = 1; copying = 0 }
			}
			candidate = 0; candidate_text = ""; signature_text = ""; next
		}
		next
	}
	copying {
		captured = captured $0 ORS
		depth += brace_delta($0)
		if (depth == 0) { found = 1; copying = 0 }
	}
	END {
		if ((definitions != 1 && alternatives != "yes") || !found || bad) exit 1
		printf "%s", captured
	}
	' "$source_file" >> "$extract"; then
		echo "could not uniquely extract expected runtime_probe.c function: $function_name" >&2
		exit 1
	fi
}

extract_struct wifi_probe_context "$source_file"
extract_struct wifi_owner_control "$source_file"
extract_struct wifi_device_runtime "$source_file"

extract_function wifi_tx_session \
	'static int wifi_tx_session(void *opaque, uint32_t generation)'
extract_function wifi_owner_now \
	'static uint32_t wifi_owner_now(void *opaque)'
extract_function wifi_owner_acquire \
	'static int wifi_owner_acquire(void *opaque, uint32_t wait_us)'
extract_function wifi_owner_remaining \
	'static ULONG wifi_owner_remaining(struct wifi_probe_context *context, ULONG started, ULONG limit)'
extract_function wifi_owner_budget \
	'static int wifi_owner_budget(void *opaque, uint32_t usec)'
extract_function wifi_owner_release \
	'static void wifi_owner_release(void *opaque)'
extract_function wifi_owner_stopping \
	'static int wifi_owner_stopping(void *opaque)'
extract_function wifi_owner_event \
	'static int wifi_owner_event(void *opaque, const struct p4_hosted_inbox_item *item)'
extract_function wifi_owner_finish_unsubmitted_reservation \
	'static int wifi_owner_finish_unsubmitted_reservation(struct p4_hosted_owner *owner, struct p4_hosted_fifo *fifo, uint32_t generation, uint32_t uid, uint32_t request_id, unsigned submitted)'
extract_function wifi_owner_request \
	'static int wifi_owner_request(struct wifi_probe_context *context, struct p4_hosted_owner *owner, struct wifi_owner_control *control, struct p4_hosted_fifo *fifo, unsigned checksum, uint32_t id, ULONG remaining)'
extract_function wifi_yield \
	'static void wifi_yield(void *opaque)'
extract_function wifi_association_target_valid \
	'static BOOL wifi_association_target_valid(const struct p4_wifi_association_target *target)'
extract_function wifi_association_ap_matches \
	'static BOOL wifi_association_ap_matches(const struct p4_wifi_association_target *target, const struct p4_hosted_sta_ap_info *ap)'
extract_function wifi_association_event_matches \
	'static BOOL wifi_association_event_matches(const struct p4_wifi_association_target *target, const struct p4_hosted_sta_ap_info *ap, const struct p4_hosted_link_observation *event, ULONG generation, ULONG connect_revision)'
extract_function wifi_device_associate \
	'static BOOL wifi_device_associate(struct P4WifiBase *base, const struct p4_wifi_association_target *target, const struct p4_wifi_association_cancel *cancel)' yes
extract_function wifi_device_disconnect \
	'static BOOL wifi_device_disconnect(struct P4WifiBase *base)' yes

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"

for diagnostics in 1 0; do
	for optimization in 0 2; do
		for peer_gate in 0 1; do
			for disconnect_fence in 0 1; do
				if [ "$diagnostics" -eq 0 ] &&
				   { [ "$optimization" -ne 0 ] || [ "$peer_gate" -ne 1 ] ||
				     [ "$disconnect_fence" -ne 1 ]; }; then
					continue
				fi
	# Rename only the real exchange implementation; retain production owner
	# cancellation/retirement, RX/FIFO checks, request IDs, and all codecs.
	# shellcheck disable=SC2086
	"$cc" $common_flags "-O$optimization" \
		-I "$test_root/network" \
		-Dp4_hosted_owner_exchange=p4_hosted_owner_exchange_unused \
		-c "$test_root/network/hosted_owner.c" -o "$owner_object"
	# shellcheck disable=SC2086
	"$cc" $common_flags "-O$optimization" \
		-DP4_BOARD_WIFI_SDIO=1 -DP4_WIFI_HOSTED_OWNER=1 -DP4_WIFI_HOSTED_REPLY=1 \
		-DP4_WIFI_ASSOC_DIAGNOSTICS="$diagnostics" \
		-DP4_WIFI_DISCONNECT_EVENT_FENCE="$disconnect_fence" \
		-DP4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED=0 \
		-DP4_WIFI_FULLMAC_PEER_CONTRACT_VERIFIED="$peer_gate" \
		-DP4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED=0 \
		-I "$test_tmp" -I "$test_root/network" -I "$test_root/sdmmc" \
		-I "$repo_root/compiler/include" \
		-I "$repo_root/workbench/tools/ESP32Board" \
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
		"$test_root/network/hosted_events.c" \
		"$test_root/network/hosted_link.c" \
		"$test_root/tools/test-wifi-owner-association.c" \
		"$owner_object" -o "$test_tmp/test-o$optimization"
	printf 'O%s peer-gate%s disconnect-fence%s diagnostics%s: ' \
		"$optimization" "$peer_gate" "$disconnect_fence" "$diagnostics"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$optimization"
			done
	done
		done
done
