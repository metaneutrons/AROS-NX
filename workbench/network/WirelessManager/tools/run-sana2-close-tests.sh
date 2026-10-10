#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SOURCE_DIR="$WM_ROOT/src"
ELOOP_SOURCE="$SOURCE_DIR/utils/eloop_amiga.c"
DRIVER_SOURCE="$SOURCE_DIR/drivers/driver_sana2.c"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sana2-close-tests.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

python3 - "$ELOOP_SOURCE" "$DRIVER_SOURCE" "$BUILD_DIR" <<'PY'
import pathlib
import re
import sys

eloop_path, driver_path, build_dir = map(pathlib.Path, sys.argv[1:])
eloop_source = eloop_path.read_text()
driver_source = driver_path.read_text()
build_dir.mkdir(parents=True, exist_ok=True)


def extract_function(text, name):
	pattern = re.compile(
		r"(?m)^(?:static\s+)?(?:void\s*\*?|int|u32)\s+"
		r"" + re.escape(name) + r"\s*\("
	)
	matches = list(pattern.finditer(text))
	if len(matches) != 1:
		raise SystemExit(
			f"error: expected one definition for {name}, found {len(matches)}"
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


def compact(text):
	return re.sub(r"\s+", " ", text)


def source_check(condition, message):
	if not condition:
		raise SystemExit(f"error: {message}")


functions = (
	("eloop_signal_mask", eloop_source),
	("eloop_handle_signals", eloop_source),
	("eloop_compact_signals", eloop_source),
	("eloop_process_pending_signals", eloop_source),
	("eloop_register_signal", eloop_source),
	("eloop_unregister_signal", eloop_source),
	("wpa_driver_sana2_deinit", driver_source),
)
for name, source in functions:
	function = extract_function(source, name)
	if name == "eloop_unregister_signal":
		function = function.replace(
			"eloop_unregister_signal(",
			"eloop_unregister_signal_impl(",
			1,
		)
	(build_dir / f"{name}.inc").write_text(function + "\n")

match = re.search(r"(?m)^#define\s+MLME_REQ_COUNT\s+(\d+)\s*$", driver_source)
source_check(match is not None, "could not read MLME_REQ_COUNT from driver")
(build_dir / "sana2-close-constants.inc").write_text(
	f"#define MLME_REQ_COUNT {match.group(1)}\n"
)

init = compact(extract_function(driver_source, "wpa_driver_sana2_init"))
deinit = compact(extract_function(driver_source, "wpa_driver_sana2_deinit"))
source_check(
	re.search(
		r"if \(eloop_register_signal\(drv->port->mp_SigBit, "
		r"wpa_driver_sana2_event_handler, drv\) != 0\) err = 1; "
		r"else drv->signal_registered = 1;",
		init,
	)
	is not None,
	"signal registration must be checked and tracked",
)
cleanup = init.find("if (err != 0) { wpa_driver_sana2_deinit(drv); drv = NULL; }")
source_check(cleanup >= 0, "init failure must route through deinit cleanup")
source_check(
	"if (drv->port != NULL) drv->request = (APTR)CreateIORequest(drv->port, sizeof(struct IOSana2Req));"
	in init,
	"init must not create an I/O request for a missing message port",
)
source_check(
	deinit.find("eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);")
	< deinit.find("eloop_unregister_signal(drv->port->mp_SigBit,")
	< deinit.find("AbortIO((APTR)drv->eapol_request);"),
	"deinit must retire the exact keepalive and signal callbacks before I/O cleanup",
)
source_check(
	deinit.find("WaitIO((APTR)drv->event_request);")
	< deinit.find("FreeVec(drv->event_request);"),
	"event request must be freed after AbortIO/WaitIO",
)
source_check(
	deinit.find("eloop_unregister_signal(drv->port->mp_SigBit,")
	< deinit.find("DeleteMsgPort(drv->port);"),
	"signal registration must be removed before deleting its port",
)
source_check(
	"SetSignal(0, (ULONG)1 << drv->port->mp_SigBit);" in deinit,
	"deinit must clear the owned port signal bit before deleting the port",
)
source_check(
	"(u32)1 << eloop.signals[i].sig" in extract_function(
		eloop_source, "eloop_signal_mask"
	),
	"signal wait mask must use an unsigned shift",
)
source_check(
	"eloop.signals[i].handler != NULL" in extract_function(
		eloop_source, "eloop_handle_signals"
	),
	"signal marking must skip tombstones",
)
PY

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -Wno-unused-parameter -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-sana2-close-O$OPT"
	$CC $CFLAGS_COMMON -O"$OPT" -I"$BUILD_DIR" \
		"$SCRIPT_DIR/test-sana2-close.c" -o "$BIN"
	echo "sana2 close host tests (-O$OPT):"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done
