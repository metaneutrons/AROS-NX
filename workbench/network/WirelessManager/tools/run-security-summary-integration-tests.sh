#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$WM_ROOT/../../.." && pwd)
SRC_DIR="$WM_ROOT/src"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/security-summary-integration.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

extract_function()
{
	source_file=$1
	function_name=$2
	output_file=$3
	awk -v name="$function_name" '
		!found && index($0, name "(") { found = 1 }
		found {
			print
			line = $0
			opened = gsub(/\{/, "", line)
			closed = gsub(/\}/, "", line)
			depth += opened - closed
			if (opened || started)
				started = 1
			if (started && depth == 0)
				exit
		}
		END {
			if (!found || !started || depth != 0)
				exit 1
		}
	' "$source_file" > "$output_file"
}

extract_define()
{
	source_file=$1
	macro_name=$2
	output_file=$3
	awk -v name="$macro_name" '
		$1 == "#define" && $2 == name { print; found++ }
		END { if (found != 1) exit 1 }
	' "$source_file" >> "$output_file"
}

extract_enum_value()
{
	source_file=$1
	enum_name=$2
	output_file=$3
	awk -v name="$enum_name" '
		index($0, name) {
			line = $0
			if (line !~ name "[[:space:]]*=")
				next
			sub("^.*" name "[[:space:]]*=[[:space:]]*", "", line)
			sub("[[:space:],].*$", "", line)
			if (line !~ /^[0-9]+$/)
				exit 1
			print "#define " name " " line
			found++
		}
		END { if (found != 1) exit 1 }
	' "$source_file" >> "$output_file"
}

PUBLIC_SANA="$REPO_ROOT/compiler/include/devices/sana2wireless.h"
PUBLIC_TAGS="$REPO_ROOT/compiler/include/utility/tagitem.h"
PUBLIC_MEMORY="$REPO_ROOT/compiler/include/exec/memory.h"
DRIVER_HEADER="$SRC_DIR/drivers/driver.h"
WPA_CONFIG="$WM_ROOT/wpa_supplicant/config_ssid.h"
AP_CONFIG="$SRC_DIR/ap/ap_config.h"
SOURCE_CONSTANTS="$BUILD_DIR/security-summary-source-constants.inc"
if [ ! -f "$PUBLIC_SANA" ] || [ ! -f "$PUBLIC_TAGS" ] ||
	[ ! -f "$PUBLIC_MEMORY" ] || [ ! -f "$DRIVER_HEADER" ] ||
	[ ! -f "$WPA_CONFIG" ] || [ ! -f "$AP_CONFIG" ]; then
	echo "error: required public or WirelessManager headers are missing" >&2
	exit 1
fi
: > "$SOURCE_CONSTANTS"
for macro in TAG_USER TAG_END; do
	extract_define "$PUBLIC_TAGS" "$macro" "$SOURCE_CONSTANTS"
done
for macro in S2WIRELESS_CAPABILITIES_VERSION S2WIRELESS_CAPF_FOURWAY \
	S2WIRELESS_CAPF_SECURITY_SUMMARY S2WIRELESS_SUMMARY_VERSION \
	S2WIRELESS_SUMF_KNOWN S2WIRELESS_SUMF_PRIVACY S2WIRELESS_PROTO_WPA \
	S2WIRELESS_PROTO_RSN S2WIRELESS_PROTO_RSN_SAE S2WIRELESS_KEY_PSK \
	S2WIRELESS_KEY_EAP S2WIRELESS_KEY_SAE S2WIRELESS_CIPHER_TKIP \
	S2WIRELESS_CIPHER_CCMP S2WIRELESS_CIPHER_GCMP S2INFO_SecuritySummary \
	S2_GETCRYPTTYPES S2ENC_WEP S2ENC_TKIP S2ENC_CCMP; do
	extract_define "$PUBLIC_SANA" "$macro" "$SOURCE_CONSTANTS"
done
for macro in MEMB_PUBLIC MEMF_PUBLIC MEMB_CLEAR MEMF_CLEAR; do
	extract_define "$PUBLIC_MEMORY" "$macro" "$SOURCE_CONSTANTS"
done
extract_define "$SRC_DIR/drivers/driver_sana2.c" PUDDLE_SIZE \
	"$SOURCE_CONSTANTS"
for macro in IEEE80211_CAP_PRIVACY WPA_DRIVER_CAPA_KEY_MGMT_WPA \
	WPA_DRIVER_CAPA_KEY_MGMT_WPA2 WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK \
	WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK WPA_DRIVER_CAPA_ENC_WEP40 \
	WPA_DRIVER_CAPA_ENC_WEP104 WPA_DRIVER_CAPA_ENC_TKIP \
	WPA_DRIVER_CAPA_ENC_CCMP WPA_DRIVER_FLAGS_USER_SPACE_MLME \
	WPA_DRIVER_FLAGS_4WAY_HANDSHAKE WPA_DRIVER_FLAGS_SECURITY_SUMMARY; do
	extract_define "$DRIVER_HEADER" "$macro" "$SOURCE_CONSTANTS"
done
extract_define "$AP_CONFIG" NUM_WEP_KEYS "$SOURCE_CONSTANTS"
extract_enum_value "$WPA_CONFIG" WPAS_MODE_INFRA "$SOURCE_CONSTANTS"

extract_function "$SRC_DIR/drivers/driver_sana2.c" \
	wpa_driver_sana2_wireless_caps_valid "$BUILD_DIR/driver_caps_valid.inc"
extract_function "$SRC_DIR/drivers/driver_sana2.c" \
	wpa_driver_sana2_set_security_summary "$BUILD_DIR/driver_set_security_summary.inc"
extract_function "$SRC_DIR/drivers/driver_sana2.c" \
	wpa_driver_sana2_get_capa "$BUILD_DIR/driver_get_capa.inc"
extract_function "$WM_ROOT/wpa_supplicant/wpa_supplicant.c" \
	wpa_supplicant_summary_profile_common "$BUILD_DIR/supplicant_summary_profile_common.inc"
extract_function "$WM_ROOT/wpa_supplicant/wpa_supplicant.c" \
	wpa_supplicant_summary_open_profile "$BUILD_DIR/supplicant_summary_open_profile.inc"
extract_function "$WM_ROOT/wpa_supplicant/wpa_supplicant.c" \
	wpa_supplicant_summary_capa_match "$BUILD_DIR/supplicant_summary_capa_match.inc"
extract_function "$WM_ROOT/wpa_supplicant/wpa_supplicant.c" \
	wpa_supplicant_summary_config_match "$BUILD_DIR/supplicant_summary_config_match.inc"
extract_function "$WM_ROOT/wpa_supplicant/events.c" \
	wpa_supplicant_summary_offload_enabled "$BUILD_DIR/events_summary_offload_enabled.inc"
extract_function "$WM_ROOT/wpa_supplicant/events.c" \
	wpa_supplicant_summary_matches_ssid "$BUILD_DIR/events_summary_matches_ssid.inc"
extract_function "$WM_ROOT/wpa_supplicant/events.c" \
	wpa_supplicant_summary_open_profile "$BUILD_DIR/events_summary_open_profile.tmp"
sed 's/wpa_supplicant_summary_open_profile/wpa_events_summary_open_profile/g' \
	"$BUILD_DIR/events_summary_open_profile.tmp" > \
	"$BUILD_DIR/events_summary_open_profile.inc"
command rm "$BUILD_DIR/events_summary_open_profile.tmp"

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -Wno-sign-compare -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-security-summary-integration-O$OPT"
	$CC $CFLAGS_COMMON -O"$OPT" \
		-I"$BUILD_DIR" -I"$SRC_DIR" -I"$SRC_DIR/common" -I"$SRC_DIR/utils" \
		"$SCRIPT_DIR/test-security-summary-integration.c" -o "$BIN"
	ASAN_OPTIONS=halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done
