#!/bin/sh
# Source-extract production maintenance/discovery/private OTA owner helpers.
# The fixture mocks Exec, codecs, UID reservation and owner transport; actual
# codec/owner suites remain separate, and no hardware behavior is claimed.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-maintenance.XXXXXX")
extract="$test_tmp/wifi_maintenance_extract.inc"
mutant_extract="$test_tmp/wifi_maintenance_mutant.inc"
cleanup_mutant_extract="$test_tmp/wifi_maintenance_cleanup_mutant.inc"
ota_mutant_extract="$test_tmp/wifi_maintenance_ota_mutant.inc"
finish_mutant_extract="$test_tmp/wifi_maintenance_finish_mutant.inc"
engine_prearm_mutant_extract="$test_tmp/wifi_maintenance_engine_prearm_mutant.inc"
clock_mutant_extract="$test_tmp/wifi_maintenance_clock_mutant.inc"
constants="$test_tmp/wifi_maintenance_constants.inc"
engine_source="$test_root/boardtool/update.c"
compiler=${CC:-cc}
compiler_diagnostics=$("$compiler" --version 2>&1 || true)
warning_suppress=
case "$compiler_diagnostics" in
	*clang*) warning_suppress=-Wno-uninitialized-const-pointer ;;
esac
source_file="$test_root/network/runtime_probe.c"
header_file="$test_root/network/device_bridge.h"
discovery_header="$test_root/network/hosted_discovery.h"
rpc_header="$test_root/network/hosted_rpc.h"
owner_header="$test_root/network/hosted_owner.h"
layout_header="$test_root/../../workbench/tools/ESP32Board/layout.h"
wire_header="$test_root/boardtool/discovery_wire.h"
init_header="$test_root/network/hosted_init.h"
ota_header="$test_root/network/hosted_ota.h"
owner_ota_header="$test_root/network/owner_ota.h"
update_header="$test_root/boardtool/update.h"
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/test-mutant" "$test_tmp/test-cleanup-mutant" "$test_tmp/test-ota-mutant" "$test_tmp/test-finish-mutant" "$test_tmp/test-engine-prearm-mutant" "$test_tmp/test-clock-mutant" "$test_tmp/mutant.out" "$test_tmp/cleanup-mutant.out" "$test_tmp/ota-mutant.out" "$test_tmp/finish-mutant.out" "$test_tmp/engine-prearm-mutant.out" "$test_tmp/clock-mutant.out" "$extract" "$mutant_extract" "$cleanup_mutant_extract" "$ota_mutant_extract" "$finish_mutant_extract" "$engine_prearm_mutant_extract" "$clock_mutant_extract" "$constants"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM" "$test_tmp/test-mutant.dSYM" "$test_tmp/test-cleanup-mutant.dSYM" "$test_tmp/test-ota-mutant.dSYM" "$test_tmp/test-finish-mutant.dSYM" "$test_tmp/test-engine-prearm-mutant.dSYM" "$test_tmp/test-clock-mutant.dSYM"; command rmdir "$test_tmp"' EXIT HUP INT TERM

if grep -Fq 'p4_wifi_owner_ota_exchange' "$header_file" ||
	grep -Fq 'p4_wifi_owner_ota_finish' "$header_file"; then
	echo "private OTA owner entry leaked into the device bridge header" >&2
	exit 1
fi
if ! grep -Fq '#include "../boardtool/update.h"' "$owner_ota_header" ||
   ! grep -Fq 'struct p4_board_update_gates' "$update_header" ||
   ! grep -Fq 'int p4_wifi_owner_ota_finish(' "$owner_ota_header"; then
	echo "private OTA owner declaration is detached from board update types" >&2
	exit 1
fi

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

extract_global_function() {
	function_name=$1
	awk -v name="$function_name" '
		!candidate && $0 ~ "^[[:space:]]*enum[[:space:]]+p4_board_update_call_result[[:space:]]+" name "[[:space:]]*\\(" {
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

extract_global_int_function() {
	function_name=$1
	awk -v name="$function_name" '
		!candidate && $0 ~ "^[[:space:]]*int[[:space:]]+" name "[[:space:]]*\\(" {
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

extract_global_status_function() {
	function_name=$1
	awk -v name="$function_name" '
		!candidate && $0 ~ "^[[:space:]]*enum[[:space:]]+p4_board_update_status[[:space:]]+" name "[[:space:]]*\\(" {
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
		echo "could not extract runtime_probe.c status function: $function_name" >&2
		exit 1
	}
}

extract_struct() {
	struct_name=$1
	awk -v name="$struct_name" '
		!copy && $0 ~ "^struct[[:space:]]+" name "[[:space:]]*\\{" { copy = 1 }
		copy {
			print
			if ($0 ~ /^};[[:space:]]*$/) { found = 1; exit }
		}
		END { if (!found) exit 1 }
	' "$source_file" >> "$extract" || {
		echo "could not extract runtime_probe.c structure: $struct_name" >&2
		exit 1
	}
}

# Exercise the production helper too, so clock wrap and expiry use the exact
# subtraction semantics compiled into runtime_probe.c.
extract_function wifi_owner_remaining
extract_function wifi_maintenance_normal_poll
extract_function wifi_maintenance_cleanup_allowed
extract_function wifi_maintenance_discovery_clear
extract_function wifi_discovery_output_valid
extract_function wifi_discovery_session_valid
extract_function wifi_ota_span_valid
extract_function wifi_ota_spans_disjoint
extract_global_function p4_wifi_owner_ota_exchange
extract_global_int_function p4_wifi_owner_ota_finish
extract_struct wifi_ota_engine_context
extract_function wifi_ota_engine_clock
extract_function wifi_ota_engine_cancelled
extract_function wifi_ota_engine_acquire
extract_function wifi_ota_engine_source
extract_function wifi_ota_engine_read
extract_function wifi_ota_engine_begin
extract_function wifi_ota_engine_write
extract_function wifi_ota_engine_end
extract_function wifi_ota_engine_finish
extract_function wifi_ota_engine_finish_unclaimed
extract_global_status_function p4_wifi_owner_ota_run
extract_function wifi_maintenance_request
extract_function wifi_maintenance_poll
extract_function wifi_maintenance_release
extract_function wifi_maintenance_fence
extract_function wifi_maintenance_discover
extract_function wifi_maintenance_discovery_poll
extract_function wifi_maintenance_discovery_service
extract_function wifi_maintenance_service

# Keep the owner-loop client callback, ordinary transport-poll, and link
# observation gates under regression coverage. A terminal fence reaches none.
awk '
	index($0, "maintenance = wifi_maintenance_service(base, &runtime);") {
		service_calls++
	}
	index($0, "if (maintenance == 1 && base->client && base->callbacks)") {
		client_service_guards++
	}
	index($0, "poll_transport = wifi_maintenance_normal_poll(base->maintenance_state);") {
		assignments++
	}
	index($0, "if ((base->maintenance_state == P4_WIFI_MAINT_IDLE ||") {
		refresh_guards++
		pending_refresh_guard = 1
	}
	{
		if (pending_refresh_guard &&
		    index($0, "base->maintenance_state == P4_WIFI_MAINT_RELEASED) &&") != 0)
			refresh_released_state++
		if (index($0, "!wifi_owner_refresh_link_snapshot(") != 0) {
			refresh_calls++
			if (!pending_refresh_guard || refresh_released_state != refresh_guards)
				bad_refresh_guard = 1
			pending_refresh_guard = 0
		}
	}
	index($0, "p4_hosted_owner_step(") {
		calls++
		if (index($0, "if (poll_transport && !p4_hosted_owner_step(") != 0)
			guarded++
	}
	END {
		if (service_calls != 1 || client_service_guards != 1 ||
		    assignments != 1 || calls != 1 || guarded != 1 ||
		    refresh_guards != 1 || refresh_released_state != 1 ||
		    refresh_calls != 1 || bad_refresh_guard) exit 1
	}
' "$source_file" || {
	echo "owner callback, transport poll, or link observation lost its maintenance gate" >&2
	exit 1
}

# Verify both protocol cleanup and the later CLOSE/restoration acquisition stay
# under the sticky cleanup predicate. Parse the actual owner-service braces so
# merely leaving the predicate elsewhere in teardown cannot satisfy the guard.
awk '
function open_delta(text, copy) {
	copy = text
	return gsub(/[{]/, "", copy)
}
function close_delta(text, copy) {
	copy = text
	return gsub(/[}]/, "", copy)
}
/^static void wifi_owner_service[(]/ { in_service = 1 }
in_service {
	if (cleanup) {
		if (index($0, "if (wifi_maintenance_cleanup_allowed(base) &&") != 0) {
			guard_count++
			pending_guard = guard_count
		}
		if ($0 ~ /checksum,[[:space:]]*281u/) {
			protocol_calls++
			if (protocol_guard_depth == 0 || depth < protocol_guard_depth)
				bad = 1
		}
		if ($0 ~ /checksum,[[:space:]]*279u/) {
			protocol_calls++
			if (protocol_guard_depth == 0 || depth < protocol_guard_depth)
				bad = 1
		}
		if (index($0, "host->acquire(host, cleanup_wait)") != 0) {
			acquire_calls++
			if (pending_guard != 2)
				bad = 1
		}
		if (index($0, "p4_hosted_open_cleanup(open_ops, opened)") != 0) {
			close_calls++
			if (close_guard_depth == 0 || depth < close_guard_depth)
				bad = 1
		}
		if (index($0, "p4_sdio_restore_function1(setup_ops, setup, restored)") != 0) {
			restore_calls++
			if (close_guard_depth == 0 || depth < close_guard_depth)
				bad = 1
		}
		lower = tolower($0)
		if (lower ~ /(reset|reboot)[[:space:]]*[(]/) reset_calls++
	}
	if (index($0, "context->owner_cleanup = TRUE;") != 0) cleanup = 1
	line = $0
	delta = open_delta(line) - close_delta(line)
	depth += delta
	if (pending_guard && open_delta(line) > 0) {
		if (pending_guard == 1) protocol_guard_depth = depth
		else if (pending_guard == 2) close_guard_depth = depth
		pending_guard = 0
	}
	if (depth == 0 && index($0, "static void wifi_owner_service(") == 0 && cleanup) {
		finished = 1
		exit
	}
}
END {
	if (!cleanup || guard_count != 2 || protocol_calls != 2 ||
	    acquire_calls != 1 || close_calls != 1 || restore_calls != 1 ||
	    reset_calls != 0 || bad || !finished) exit 1
}
' "$source_file" || {
	echo "owner teardown protocol/CLOSE/restoration escaped the sticky fence or added reset" >&2
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
awk '$1 == "#define" && $2 == "P4_HOSTED_OTA_MAX_WRITE_SIZE" { print }' \
	"$ota_header" >> "$constants"
awk '$1 == "#define" && $2 == "P4_HOSTED_OWNER_MAX_EXCHANGE_US" { print }' \
	"$owner_header" >> "$constants"
awk '$1 == "#define" && ($2 == "P4_HOSTED_INIT_CHIP_ESP32C6" ||
	$2 == "P4_HOSTED_INIT_CAP_WLAN_SDIO") { print }' \
	"$init_header" >> "$constants"
awk '/^enum p4_hosted_init_present \{/ { copy = 1 }
	copy { print }
	copy && /^\};/ { exit }' "$init_header" >> "$constants"

for optimization in 0 2; do
	"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-O"$optimization" -I "$test_tmp" -I "$test_root/network" \
		"$test_root/tools/test-wifi-maintenance.c" "$engine_source" \
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
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_MAINTENANCE_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-mutant"
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

# Negative control: if the terminal cleanup predicate is widened to always
# allow cleanup, the sticky-mode assertions must reject it.
sed 's/return base->maintenance_fence_mode == 0;/return base != NULL;/' \
	"$extract" > "$cleanup_mutant_extract"
if ! grep -Fq 'return base != NULL;' "$cleanup_mutant_extract" ||
   grep -Fq 'return base->maintenance_fence_mode == 0;' "$cleanup_mutant_extract"; then
	echo "could not construct cleanup-allowed mutation" >&2
	exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_MAINTENANCE_CLEANUP_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-cleanup-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
   UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-cleanup-mutant" > "$test_tmp/cleanup-mutant.out" 2>&1; then
	echo "always-allowed cleanup mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq '!wifi_maintenance_cleanup_allowed(&base)' \
	"$test_tmp/cleanup-mutant.out"; then
	echo "cleanup mutation failed for a reason other than the sticky predicate assertion" >&2
	cat "$test_tmp/cleanup-mutant.out" >&2
	exit 1
fi
echo "wifi maintenance sticky-cleanup mutation rejected"

# A missing pre-BEGIN sticky fence must be rejected before any native OTA call
# can reach the owner. The mutation affects only the extracted test copy.
sed 's/^[[:space:]]*base->maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;/    \/\* missing pre-BEGIN fence mutant \*\//' \
	"$extract" > "$ota_mutant_extract"
if grep -Eq '^[[:space:]]*base->maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;' \
	"$ota_mutant_extract"; then
	echo "could not construct missing pre-BEGIN fence mutation" >&2
	exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_OTA_PREBEGIN_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-ota-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-ota-mutant" > "$test_tmp/ota-mutant.out" 2>&1; then
	echo "missing pre-BEGIN fence mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq 'base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE' \
	"$test_tmp/ota-mutant.out"; then
	echo "OTA mutation failed for a reason other than the pre-BEGIN sticky assertion" >&2
	cat "$test_tmp/ota-mutant.out" >&2
	exit 1
fi
echo "wifi OTA pre-BEGIN sticky-fence mutation rejected"

# Explicit engine/native mode mapping must not regress to enum casting.
sed 's/case P4_BOARD_UPDATE_FINISH_NO_BEGIN: native_mode = 0; break;/case P4_BOARD_UPDATE_FINISH_NO_BEGIN: native_mode = (ULONG)mode; break;/' \
	"$extract" > "$finish_mutant_extract"
if ! grep -Fq 'case P4_BOARD_UPDATE_FINISH_NO_BEGIN: native_mode = (ULONG)mode; break;' \
	"$finish_mutant_extract"; then
	echo "could not construct OTA finish enum-cast mutation" >&2
	exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_OTA_FINISH_CAST_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-finish-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-finish-mutant" > "$test_tmp/finish-mutant.out" 2>&1; then
	echo "OTA finish enum-cast mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq 'base.maintenance_fence_mode == cases[index].native_mode' \
	"$test_tmp/finish-mutant.out"; then
	echo "finish mapping mutation failed for an unrelated reason" >&2
	cat "$test_tmp/finish-mutant.out" >&2
	exit 1
fi
grep -F 'base.maintenance_fence_mode == cases[index].native_mode' \
	"$test_tmp/finish-mutant.out"
echo "wifi OTA finish enum-cast mutation rejected"

# The engine adapter, not the native exchange entry, must arm before entering
# BEGIN local preflight. The source-clock assertion rejects a missing arm.
sed '/wifi_ota_engine_begin(/,/^}/s/c->base->maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;/\/\* missing engine BEGIN prearm mutant \*\//' \
	"$extract" > "$engine_prearm_mutant_extract"
if grep -Fq 'c->base->maintenance_fence_mode = P4_WIFI_MAINT_FENCE_INCOMPLETE;' \
	"$engine_prearm_mutant_extract"; then
	echo "could not construct missing engine BEGIN prearm mutation" >&2
	exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_OTA_ENGINE_PREARM_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-engine-prearm-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-engine-prearm-mutant" > "$test_tmp/engine-prearm-mutant.out" 2>&1; then
	echo "missing engine BEGIN prearm mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq 'base.maintenance_fence_mode == P4_WIFI_MAINT_FENCE_INCOMPLETE' \
	"$test_tmp/engine-prearm-mutant.out"; then
	echo "engine prearm mutation failed for an unrelated reason" >&2
	cat "$test_tmp/engine-prearm-mutant.out" >&2
	exit 1
fi
echo "wifi OTA engine pre-BEGIN prearm mutation rejected"

# The aggregate clock must retain maintenance_started as its first sample.
sed 's/ : c->started;/ : c->runtime->context->host->now_us();/' \
	"$extract" > "$clock_mutant_extract"
if ! grep -Fq '*now = c->clock_started ? c->runtime->context->host->now_us() : c->runtime->context->host->now_us();' \
	"$clock_mutant_extract"; then
	echo "could not construct renewed-clock mutation" >&2
	exit 1
fi
"$compiler" -std=c99 -Wall -Wextra -Werror -pedantic $warning_suppress -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer -O0 \
	-DWIFI_OTA_CLOCK_RENEWAL_MUTANT -I "$test_tmp" -I "$test_root/network" \
	"$test_root/tools/test-wifi-maintenance.c" "$engine_source" -o "$test_tmp/test-clock-mutant"
if ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$test_tmp/test-clock-mutant" > "$test_tmp/clock-mutant.out" 2>&1; then
	echo "renewed-clock mutation unexpectedly passed" >&2
	exit 1
fi
if ! grep -Fq '== P4_BOARD_UPDATE_BUDGET_EXPIRED' "$test_tmp/clock-mutant.out"; then
	echo "renewed-clock mutation failed for an unrelated reason" >&2
	cat "$test_tmp/clock-mutant.out" >&2
	exit 1
fi
echo "wifi OTA original-deadline mutation rejected"
