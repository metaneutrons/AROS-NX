#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$WM_ROOT/../../.." && pwd)
SRC_DIR="$WM_ROOT/src"
SOURCE="$SRC_DIR/drivers/driver_sana2.c"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sana2-connect-failed-tests.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

python3 - "$SOURCE" "$REPO_ROOT/compiler/include" \
	"$SRC_DIR/drivers/driver.h" "$BUILD_DIR" <<'PY'
import pathlib
import re
import sys

source_path, include_root, driver_header, build_dir = map(pathlib.Path, sys.argv[1:])
source = source_path.read_text()


def extract_function(text, name):
	pattern = re.compile(
		r"(?m)^static\s+[A-Za-z_][A-Za-z0-9_ \t*]*?\b"
		+ re.escape(name)
		+ r"\s*\("
	)
	matches = list(pattern.finditer(text))
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one static definition for {name}, found {len(matches)}"
		)
	start = matches[0].start()
	state = "code"
	depth = 0
	opened = False
	i = start
	while i < len(text):
		char = text[i]
		next_char = text[i + 1] if i + 1 < len(text) else ""
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
					return text[start : i + 1]
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


def extract_define(path, name):
	pattern = re.compile(
		r"^\s*#\s*define\s+" + re.escape(name) + r"(?:\s+|$)(.*)$"
	)
	matches = []
	for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
		match = pattern.match(line)
		if match:
			value = match.group(1).split("/*", 1)[0].split("//", 1)[0].strip()
			if not value:
				raise SystemExit(f"error: empty definition for {name} in {path}")
			matches.append(value)
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one definition for {name} in {path}, found {len(matches)}"
		)
	return f"#define {name} {matches[0]}"


def extract_enum_value(path, enum_name, member):
	text = path.read_text(encoding="utf-8", errors="replace")
	pattern = re.compile(r"enum\s+" + re.escape(enum_name) + r"\s*\{([^}]*)\}", re.S)
	blocks = list(pattern.finditer(text))
	if len(blocks) != 1:
		raise SystemExit(
			f"error: expected one enum {enum_name} in {path}, found {len(blocks)}"
		)
	body = re.sub(r"/\*.*?\*/|//[^\n]*", "", blocks[0].group(1), flags=re.S)
	value = -1
	found = []
	for item in body.split(","):
		item = item.strip()
		if not item:
			continue
		if "=" in item:
			name, expression = map(str.strip, item.split("=", 1))
			try:
				value = int(expression, 0)
			except ValueError:
				raise SystemExit(
					f"error: cannot resolve enum {enum_name}.{name} = {expression}"
				)
		else:
			name = item
			value += 1
		if name == member:
			found.append(value)
	if len(found) != 1:
		raise SystemExit(
			f"error: expected one enum member {enum_name}.{member}, found {len(found)}"
		)
	return f"#define {member} {found[0]}"


def extract_braced_body(text, opening_brace):
	state = "code"
	depth = 0
	i = opening_brace
	while i < len(text):
		char = text[i]
		next_char = text[i + 1] if i + 1 < len(text) else ""
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
				depth += 1
			elif char == "}":
				depth -= 1
				if depth == 0:
					return text[opening_brace + 1 : i]
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
	raise SystemExit("error: incomplete source block")


include_root = pathlib.Path(include_root)
public_sana = include_root / "devices/sana2.h"
public_wireless = include_root / "devices/sana2wireless.h"
constants = []
for macro in (
	"S2ERR_NOT_SUPPORTED",
	"S2EVENT_OFFLINE",
	"S2EVENT_CONNECT",
	"S2EVENT_DISCONNECT",
	"S2EVENT_CONNECT_FAILED",
):
	constants.append(extract_define(public_sana, macro))
for macro in (
	"S2WIRELESS_CAPABILITIES_VERSION",
	"S2WIRELESS_CAPF_FOURWAY",
	"S2WIRELESS_CAPF_SECURITY_SUMMARY",
	"S2WIRELESS_CAPF_RAW_PSK",
	"S2WIRELESS_CAPF_IE_EXTENT",
	"S2WIRELESS_CAPF_CONNECT_FAILED",
	"S2WIRELESS_PROTO_WPA",
	"S2WIRELESS_PROTO_RSN",
	"S2WIRELESS_PROTO_RSN_SAE",
	"S2WIRELESS_KEY_PSK",
	"S2WIRELESS_KEY_EAP",
	"S2WIRELESS_KEY_SAE",
	"S2WIRELESS_CIPHER_TKIP",
	"S2WIRELESS_CIPHER_CCMP",
	"S2WIRELESS_CIPHER_GCMP",
	"S2_GETWIRELESSCAPABILITIES",
):
	constants.append(extract_define(public_wireless, macro))
constants.append(extract_enum_value(driver_header, "wpa_event_type", "EVENT_ASSOC_FAILED"))

build_dir = pathlib.Path(build_dir)
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "sana2-connect-failed-source-constants.inc").write_text(
	"\n".join(constants) + "\n"
)
for name, filename in (
	("wpa_driver_sana2_wireless_caps_valid", "sana2-caps-valid.inc"),
	("wpa_driver_sana2_query_wireless_caps", "sana2-query-caps.inc"),
	("wpa_driver_sana2_initial_event_mask", "sana2-initial-event-mask.inc"),
	("wpa_driver_sana2_connect_failed", "sana2-connect-failed.inc"),
):
	(build_dir / filename).write_text(extract_function(source, name) + "\n")

handler = extract_function(source, "wpa_driver_sana2_event_handler")
for event in ("S2EVENT_DISCONNECT", "S2EVENT_OFFLINE"):
	match = re.search(r"if\s*\(\s*events\s*&\s*" + event + r"\s*\)\s*\{", handler)
	if match is None:
		raise SystemExit(f"error: event handler has no {event} branch")
	body = extract_braced_body(handler, handler.find("{", match.start()))
	if "drv->active_association_cookie = 0;" not in body:
		raise SystemExit(f"error: {event} branch does not clear the active association cookie")
connect_match = re.search(
	r"if\s*\(\s*\(\s*events\s*&\s*S2EVENT_CONNECT\s*\)\s*&&\s*"
	r"\((?P<guard>.*?)\)\s*\)\s*\{",
	handler,
	re.S,
)
if connect_match is None:
	raise SystemExit("error: event handler CONNECT branch lacks its cookie gate")
connect_gate = re.search(
	r"!\s*\(\s*drv->wireless_caps_valid\s*&&\s*"
	r"\(\s*drv->wireless_caps\.Flags\s*&\s*"
	r"S2WIRELESS_CAPF_CONNECT_FAILED\s*\)\s*\)\s*\|\|\s*"
	r"drv->active_association_cookie\s*!=\s*0",
	connect_match.group("guard"),
	re.S,
)
if connect_gate is None:
	raise SystemExit(
		"error: negotiated CONNECT_FAILED CONNECT is not gated on a nonzero active cookie"
	)
connect_body = extract_braced_body(handler, handler.find("{", connect_match.start()))
if "drv->active_association_cookie = 0;" not in connect_body:
	raise SystemExit("error: accepted CONNECT does not clear the active association cookie")
if re.search(
	r"if\s*\(\s*events\s*&\s*S2EVENT_CONNECT_FAILED\s*\)\s*"
	r"wpa_driver_sana2_connect_failed\s*\(\s*drv\s*,\s*request\s*\)",
	handler,
) is None:
	raise SystemExit("error: event handler does not route CONNECT_FAILED to its helper")
rearm_index = handler.rfind("request->ios2_Req.io_Command = S2_ONEVENT")
length_reset_index = handler.find("request->ios2_DataLength = 0;", rearm_index)
if rearm_index < 0 or length_reset_index < 0:
	raise SystemExit("error: rearmed S2_ONEVENT request does not clear DataLength")

init_function = extract_function(source, "wpa_driver_sana2_init")
mask_index = init_function.find("wpa_driver_sana2_initial_event_mask(drv)")
arm_index = init_function.find("request->ios2_Req.io_Command = S2_ONEVENT")
if mask_index < 0 or arm_index < 0 or mask_index >= arm_index:
	raise SystemExit("error: capability mask is not computed before S2_ONEVENT is armed")
if "request->ios2_DataLength = 0;" not in init_function[arm_index:]:
	raise SystemExit("error: initial S2_ONEVENT request does not clear DataLength")

PY

echo "sana2 event source assertions (CONNECT cookie gate is source-only): passed"

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -Wno-unused-parameter -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-sana2-connect-failed-O$OPT"
	$CC $CFLAGS_COMMON -O"$OPT" -I"$BUILD_DIR" \
		"$SCRIPT_DIR/test-sana2-connect-failed.c" -o "$BIN"
	echo "sana2 connect-failed host tests (-O$OPT):"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done
