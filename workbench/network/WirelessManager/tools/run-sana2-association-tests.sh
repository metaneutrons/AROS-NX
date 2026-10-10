#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$WM_ROOT/../../.." && pwd)
SRC_DIR="$WM_ROOT/src"
SOURCE="$SRC_DIR/drivers/driver_sana2.c"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sana2-association-tests.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

python3 - "$SOURCE" "$REPO_ROOT/compiler/include" "$SRC_DIR" \
	"$BUILD_DIR" <<'PY'
import pathlib
import re
import sys

source_path, include_root, src_root, build_dir = map(pathlib.Path, sys.argv[1:])
source_text = source_path.read_text()


def extract_function(text, name):
	pattern = re.compile(r"(?m)^static int\s+" + re.escape(name) + r"\s*\(")
	matches = list(pattern.finditer(text))
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one static int definition for {name}, found {len(matches)}"
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
	pattern = re.compile(r"^\s*#\s*define\s+" + re.escape(name) + r"(?:\s+|$)(.*)$")
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
	return found[0]


public_tags = include_root / "utility/tagitem.h"
public_sana2 = include_root / "devices/sana2.h"
public_wireless = include_root / "devices/sana2wireless.h"
public_errors = include_root / "exec/errors.h"
driver_header = src_root / "drivers/driver.h"
common_defs = src_root / "common/defs.h"
for required in (
	public_tags,
	public_sana2,
	public_wireless,
	public_errors,
	driver_header,
	common_defs,
):
	if not required.is_file():
		raise SystemExit(f"error: required source/header is missing: {required}")

constants = []
constants.append(extract_define(public_tags, "TAG_USER"))
for macro in ("TAG_END", "TAG_IGNORE"):
	constants.append(extract_define(public_tags, macro))
constants.append(extract_define(source_path, "MAX_SSID_LEN"))
for macro in (
	"S2INFO_SSID",
	"S2INFO_BSSID",
	"S2INFO_AuthTypes",
	"S2INFO_PortType",
	"S2INFO_Channel",
	"S2INFO_WPAInfo",
	"S2INFO_Passphrase",
	"S2_SETOPTIONS",
	"S2_SETKEY",
	"S2WIRELESS_CAPF_FOURWAY",
	"S2ENC_NONE",
	"S2ENC_WEP",
	"S2ENC_TKIP",
	"S2ENC_CCMP",
	"S2PORT_MANAGED",
	"S2PORT_ADHOC",
):
	constants.append(extract_define(public_wireless, macro))
constants.append(extract_define(public_sana2, "S2ERR_NOT_SUPPORTED"))
constants.append(extract_define(public_errors, "IOERR_NOCMD"))
for macro in ("IEEE80211_MODE_INFRA", "IEEE80211_MODE_IBSS"):
	constants.append(extract_define(driver_header, macro))

constants.append(
	f"#define TEST_WPA_ALG_NONE {extract_enum_value(common_defs, 'wpa_alg', 'WPA_ALG_NONE')}"
)
constants.append(
	f"#define TEST_WPA_ALG_WEP {extract_enum_value(common_defs, 'wpa_alg', 'WPA_ALG_WEP')}"
)
for member in ("CIPHER_NONE", "CIPHER_WEP40", "CIPHER_TKIP", "CIPHER_CCMP", "CIPHER_WEP104"):
	constants.append(
		f"#define {member} {extract_enum_value(common_defs, 'wpa_cipher', member)}"
	)
constants.append(
	f"#define KEY_MGMT_PSK {extract_enum_value(common_defs, 'wpa_key_mgmt', 'KEY_MGMT_PSK')}"
)

build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "sana2-association-source-constants.inc").write_text(
	"\n".join(constants) + "\n"
)
(build_dir / "sana2-set-key.inc").write_text(
	extract_function(source_text, "wpa_driver_sana2_set_key") + "\n"
)
(build_dir / "sana2-cipher.inc").write_text(
	extract_function(source_text, "wpa_driver_sana2_cipher") + "\n"
)
(build_dir / "sana2-associate.inc").write_text(
	extract_function(source_text, "wpa_driver_sana2_associate") + "\n"
)
PY

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -Wno-sign-compare -Wno-unused-parameter -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-sana2-association-O$OPT"
	$CC $CFLAGS_COMMON -O"$OPT" -I"$BUILD_DIR" \
		"$SCRIPT_DIR/test-sana2-association.c" -o "$BIN"
	echo "sana2 association host tests (-O$OPT):"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done

if command -v shasum >/dev/null 2>&1; then
	echo "source SHA-256: $(shasum -a 256 "$SOURCE" | awk '{print $1}')"
else
	echo "source SHA-256: $(sha256sum "$SOURCE" | awk '{print $1}')"
fi
