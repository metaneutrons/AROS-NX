#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SRC_DIR="$WM_ROOT/src"
EVENTS_SOURCE="$WM_ROOT/wpa_supplicant/events.c"
SUPPLICANT_SOURCE="$WM_ROOT/wpa_supplicant/wpa_supplicant.c"
DRIVER_HEADER="$SRC_DIR/drivers/driver.h"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sana2-assoc-failed-state.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

python3 - "$EVENTS_SOURCE" "$SUPPLICANT_SOURCE" \
	"$DRIVER_HEADER" "$BUILD_DIR" <<'PY'
import pathlib
import re
import sys

events_path, supplicant_path, driver_header, build_dir = map(
	pathlib.Path, sys.argv[1:]
)

events = events_path.read_text()
supplicant = supplicant_path.read_text()
driver = driver_header.read_text()


def extract_function(source, name):
	pattern = re.compile(
		r"(?m)^(?:static\s+)?void\s+" + re.escape(name) + r"\s*\("
	)
	matches = list(pattern.finditer(source))
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one definition for {name}, found {len(matches)}"
		)
	start = matches[0].start()
	state = "code"
	depth = 0
	opened = False
	i = start
	while i < len(source):
		char = source[i]
		next_char = source[i + 1] if i + 1 < len(source) else ""
		if state == "code":
			if char == "/" and next_char == "*":
				state = "comment"
				i += 2
				continue
			if char == "/" and next_char == "/":
				state = "line-comment"
				i += 2
				continue
			if char == '"':
				state = "string"
			elif char == "'":
				state = "char"
			elif char == "{":
				opened = True
				depth += 1
			elif char == "}":
				depth -= 1
				if opened and depth == 0:
					return source[start : i + 1]
		elif state == "comment":
			if char == "*" and next_char == "/":
				state = "code"
				i += 2
				continue
		elif state == "line-comment":
			if char == "\n":
				state = "code"
		elif state in ("string", "char"):
			if char == "\\":
				i += 2
				continue
			if (state == "string" and char == '"') or (
				state == "char" and char == "'"
			):
				state = "code"
		i += 1
	raise SystemExit(f"error: incomplete function while extracting {name}")


dispatch_case = re.compile(
	r"(?m)^\s*case\s+EVENT_ASSOC_FAILED\s*:\s*"
	r"wpa_supplicant_event_assoc_failed\(wpa_s\);\s*break\s*;"
)
if len(dispatch_case.findall(events)) != 1:
	raise SystemExit(
		"error: events.c must dispatch EVENT_ASSOC_FAILED to the generic handler"
	)
if len(re.findall(r"(?m)^\s*EVENT_ASSOC_FAILED\s*,?\s*$", driver)) != 1:
	raise SystemExit("error: driver.h must declare EVENT_ASSOC_FAILED exactly once")

flag_names = (
	"WPA_DRIVER_FLAGS_USER_SPACE_MLME",
	"WPA_DRIVER_FLAGS_CANCEL_PENDING",
)
flag_lines = []
for name in flag_names:
	matches = re.findall(
		r"(?m)^\s*#\s*define\s+" + re.escape(name) + r"\s+[^\n]+",
		driver,
	)
	if len(matches) != 1:
		raise SystemExit(f"error: expected one definition for {name}")
	flag_lines.append(matches[0].strip())
(build_dir / "sana2-assoc-failed-state-constants.inc").write_text(
	"\n".join(flag_lines) + "\n"
)

functions = [
	extract_function(events, "wpa_supplicant_mark_disassoc"),
	extract_function(supplicant, "wpa_supplicant_cancel_auth_timeout_no_blacklist"),
	extract_function(supplicant, "wpa_supplicant_cancel_auth_timeout"),
	extract_function(supplicant, "wpa_supplicant_disassociate"),
	extract_function(supplicant, "wpa_supplicant_deauthenticate"),
	extract_function(events, "wpa_supplicant_event_assoc_failed"),
]
(build_dir / "sana2-assoc-failed-state-functions.inc").write_text(
	"\n\n".join(functions) + "\n"
)
PY

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-sana2-assoc-failed-state-O$OPT"
	$CC $CFLAGS_COMMON -O"$OPT" \
		-I"$SRC_DIR" -I"$SRC_DIR/common" \
		-I"$BUILD_DIR" \
		"$SCRIPT_DIR/test-sana2-assoc-failed-state.c" -o "$BIN"
	ASAN_OPTIONS=halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done
