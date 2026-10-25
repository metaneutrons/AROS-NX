#!/usr/bin/env python3
"""Compile production SAE config routines against small bounded mocks."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
WM = ROOT / "workbench/network/WirelessManager"
CONFIG_C = WM / "wpa_supplicant/config.c"
CONFIG_FILE_C = WM / "wpa_supplicant/config_file.c"


def extract_function(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(?m)^(?:static\s+)?[A-Za-z_][A-Za-z_0-9\s\*]*?\b{name}\s*\("
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise AssertionError(f"expected one definition of {name}, got {len(matches)}")

    start = matches[0].start()
    brace = source.find("{", matches[0].end())
    if brace < 0:
        raise AssertionError(f"no function body for {name}")

    depth = 0
    state = "normal"
    escaped = False
    i = brace
    while i < len(source):
        char = source[i]
        next_char = source[i + 1] if i + 1 < len(source) else ""
        if state == "normal":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and next_char == "/":
                state = "line_comment"
                i += 1
            elif char == "/" and next_char == "*":
                state = "block_comment"
                i += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "normal"
        elif state == "line_comment":
            if char == "\n":
                state = "normal"
        elif state == "block_comment" and char == "*" and next_char == "/":
            state = "normal"
            i += 1
        i += 1
    raise AssertionError(f"unterminated function body for {name}")


def source_contract_checks(
    config: str, config_file: str, defs: str, config_ssid: str
) -> None:
    checks = {
        "SAE is a separate key-management bit": "#define WPA_KEY_MGMT_SAE BIT(10)",
        "SAE enum is appended without renumbering existing suites":
            "KEY_MGMT_WPS,\n\tKEY_MGMT_SAE",
        "SAE key-management parser token": 'os_strcmp(start, "SAE") == 0',
        "SAE key-management writer token": '"%sSAE"',
        "SAE password is registered as key material":
            "{ FUNC_KEY(sae_password) }",
        "SAE credential has its own pointer and explicit length":
            "u8 *sae_password;\n\tsize_t sae_password_len;",
        "diagnostic consent has no inferred default":
            "ssid->fullmac_allow_diagnostic = 0;",
        "diagnostic consent uses the actual custom table writer":
            "{ FUNC(fullmac_allow_diagnostic) }",
        "custom diagnostic writer does not use table offsets":
            'os_snprintf(value, 20, "%d", ssid->fullmac_allow_diagnostic)',
        "SAE failures redact key values":
            'field->key_data ?\n\t\t\t\t\t   "[KEY DATA REMOVED]" : value',
        "hidden key query wipes its serialized SAE secret":
            'wpa_config_free_sae_password_data(\n\t\t\t\t\t\t\t(u8 *) res, os_strlen(res));',
        "SAE writer is persisted by the network writer":
            "write_sae_password(f, ssid);",
        "diagnostic field is persisted by the network writer":
            "INT(fullmac_allow_diagnostic);",
        "network parser does not log malformed option contents":
            '"Line %d: invalid network option line."',
    }
    combined = config + "\n" + config_file + "\n" + defs + "\n" + config_ssid
    for label, needle in checks.items():
        if needle not in combined:
            raise AssertionError(f"source contract failed: {label}")
    if "WPA_KEY_MGMT_SAE" in extract_function(
        defs, "wpa_key_mgmt_wpa_psk"
    ):
        raise AssertionError("SAE must not be classified as a PSK suite")


HARNESS_PREFIX = r'''#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;
#define BIT(n) (1U << (n))
#include "common/defs.h"

#define MSG_ERROR 1
#define MSG_DEBUG 2
#define MSG_MSGDUMP 3
#define NUM_WEP_KEYS 4
#define WPAS_MODE_INFRA 0
#define WPAS_MODE_IBSS 1
#define DEFAULT_PROTO (WPA_PROTO_WPA | WPA_PROTO_RSN)
#define DEFAULT_PAIRWISE (WPA_CIPHER_CCMP | WPA_CIPHER_TKIP)
#define DEFAULT_GROUP (WPA_CIPHER_CCMP | WPA_CIPHER_TKIP | WPA_CIPHER_WEP104 | WPA_CIPHER_WEP40)
#define DEFAULT_KEY_MGMT (WPA_KEY_MGMT_PSK | WPA_KEY_MGMT_IEEE8021X)

struct wpa_ssid;
struct parse_data {
    char *name;
    int (*parser)(const struct parse_data *, struct wpa_ssid *, int, const char *);
    char *(*writer)(const struct parse_data *, struct wpa_ssid *);
    void *param1, *param2, *param3, *param4;
    int key_data;
};

struct wpa_ssid {
    u8 *ssid;
    char *passphrase;
    u8 *sae_password;
    size_t sae_password_len;
    int pairwise_cipher;
    int group_cipher;
    int key_mgmt;
    int proto;
    int mode;
    int psk_set;
    size_t wep_key_len[NUM_WEP_KEYS];
    char *id_str;
    char *scan_freq;
    char *freq_list;
    char *bgscan;
    int fullmac_allow_diagnostic;
};

static char captured_logs[8192];
static size_t captured_logs_len;
static unsigned int pbkdf2_calls;
static u8 *watch_ptr;
static size_t watch_len;
static int watch_enabled;
static int watch_observed;
static int watch_all_zero;
static int watch_next_decoded_secret;
static u8 *last_decoded;
static unsigned int os_allocations;
static unsigned int os_releases;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

static void watch_memory(u8 *ptr, size_t len)
{
    watch_ptr = ptr;
    watch_len = len;
    watch_enabled = 1;
    watch_observed = 0;
    watch_all_zero = 1;
}

static void os_free(void *ptr)
{
    size_t i;
    if (ptr != NULL && watch_enabled && ptr == watch_ptr) {
        for (i = 0; i <= watch_len; i++)
            if (((u8 *) ptr)[i] != 0)
                watch_all_zero = 0;
        watch_observed = 1;
        watch_enabled = 0;
    }
    if (ptr != NULL)
        ++os_releases;
    free(ptr);
}

static void *os_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr != NULL)
        ++os_allocations;
    return ptr;
}
static void *os_zalloc(size_t size)
{
    void *ptr = calloc(1, size);
    if (ptr != NULL)
        ++os_allocations;
    return ptr;
}
static char *os_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = os_malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

#define os_memcpy memcpy
#define os_memset memset
#define os_strlen strlen
#define os_strcmp strcmp
#define os_strncmp strncmp
#define os_strchr strchr
#define os_strrchr strrchr
#define os_snprintf snprintf

static void wpa_printf(int level, const char *format, ...)
{
    va_list ap;
    int n;
    (void) level;
    if (captured_logs_len >= sizeof(captured_logs) - 1)
        return;
    va_start(ap, format);
    n = vsnprintf(captured_logs + captured_logs_len,
                  sizeof(captured_logs) - captured_logs_len, format, ap);
    va_end(ap);
    if (n > 0)
        captured_logs_len += (size_t) n;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexstr2bin(const char *hex, u8 *out, size_t len)
{
    size_t i;
    if (watch_next_decoded_secret) {
        last_decoded = out;
        watch_memory(out, len);
        watch_next_decoded_secret = 0;
    }
    for (i = 0; i < len; i++) {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (u8) ((hi << 4) | lo);
    }
    return 0;
}

static void wpa_snprintf_hex(char *buf, size_t buf_len,
                             const u8 *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (buf_len == 0)
        return;
    for (i = 0; i < len && 2 * i + 2 <= buf_len; i++) {
        buf[2 * i] = digits[data[i] >> 4];
        buf[2 * i + 1] = digits[data[i] & 15];
    }
    buf[2 * i] = '\0';
}

static void wpa_config_update_psk(struct wpa_ssid *ssid)
{
    ++pbkdf2_calls;
    ssid->psk_set = 1;
}

static char *wpa_config_get(struct wpa_ssid *ssid, const char *field);

'''


def main() -> int:
    config = CONFIG_C.read_text()
    config_file = CONFIG_FILE_C.read_text()
    defs = (WM / "src/common/defs.h").read_text()
    config_ssid = (WM / "wpa_supplicant/config_ssid.h").read_text()
    source_contract_checks(config, config_file, defs, config_ssid)

    functions = [
        (config, "wpa_config_parse_string"),
        (config, "wpa_config_free_sae_password_data"),
        (config, "wpa_config_clear_sae_password"),
        (config, "wpa_config_sae_hex_digit"),
        (config, "wpa_config_parse_sae_password"),
        (config, "wpa_config_parse_key_mgmt"),
        (config, "wpa_config_write_key_mgmt"),
        (config, "wpa_config_parse_fullmac_allow_diagnostic"),
        (config, "wpa_config_write_fullmac_allow_diagnostic"),
        (config, "wpa_config_set_network_defaults"),
        (config, "wpa_config_free_ssid"),
        (config, "is_hex"),
        (config, "wpa_config_write_string_ascii"),
        (config, "wpa_config_write_string_hex"),
        (config, "wpa_config_write_string"),
        (config, "wpa_config_write_sae_password"),
        (config_file, "wpa_config_validate_network"),
        (config_file, "write_sae_password"),
        (config_file, "write_key_mgmt"),
        (config_file, "write_int"),
    ]
    extracted = "\n\n".join(extract_function(src, name) for src, name in functions)

    harness = r'''static char *wpa_config_get(struct wpa_ssid *ssid, const char *field)
{
    if (strcmp(field, "sae_password") == 0)
        return wpa_config_write_sae_password(NULL, ssid);
    if (strcmp(field, "key_mgmt") == 0)
        return wpa_config_write_key_mgmt(NULL, ssid);
    if (strcmp(field, "fullmac_allow_diagnostic") == 0)
        return wpa_config_write_fullmac_allow_diagnostic(NULL, ssid);
    return NULL;
}

static char *read_stream(FILE *f)
{
    long n;
    char *buf;
    fflush(f);
    check(fseek(f, 0, SEEK_END) == 0, "seek output stream");
    n = ftell(f);
    check(n >= 0, "tell output stream");
    check(fseek(f, 0, SEEK_SET) == 0, "rewind output stream");
    buf = malloc((size_t) n + 1);
    check(buf != NULL, "allocate output copy");
    check(fread(buf, 1, (size_t) n, f) == (size_t) n, "read output stream");
    buf[n] = '\0';
    return buf;
}

static void profile_sae(struct wpa_ssid *ssid)
{
    memset(ssid, 0, sizeof(*ssid));
    wpa_config_set_network_defaults(ssid);
    ssid->key_mgmt = WPA_KEY_MGMT_SAE;
    ssid->proto = WPA_PROTO_RSN;
    ssid->pairwise_cipher = WPA_CIPHER_CCMP;
    ssid->group_cipher = WPA_CIPHER_CCMP;
    ssid->mode = WPAS_MODE_INFRA;
    check(wpa_config_parse_sae_password(NULL, ssid, 1,
                                        "\"NeverLogSecret\"") == 0,
          "parse valid SAE password");
}

static void test_parser_writer_and_key_mgmt(void)
{
    struct wpa_ssid ssid;
    struct parse_data no_offset;
    char *value;

    memset(&ssid, 0, sizeof(ssid));
    wpa_config_set_network_defaults(&ssid);
    check(ssid.fullmac_allow_diagnostic == 0,
          "diagnostic consent defaults to zero");
    check(wpa_config_parse_fullmac_allow_diagnostic(NULL, &ssid, 1, "1") == 0,
          "parse diagnostic opt-in");
    check(ssid.fullmac_allow_diagnostic == 1, "diagnostic opt-in stored");
    memset(&no_offset, 0, sizeof(no_offset));
    value = wpa_config_write_fullmac_allow_diagnostic(&no_offset, &ssid);
    check(value != NULL && strcmp(value, "1") == 0,
          "custom diagnostic writer ignores absent table offset");
    os_free(value);
    check(wpa_config_parse_fullmac_allow_diagnostic(NULL, &ssid, 1, "01") < 0,
          "diagnostic parser rejects noncanonical integer");
    check(wpa_config_parse_fullmac_allow_diagnostic(NULL, &ssid, 1, "2") < 0,
          "diagnostic parser rejects out-of-range integer");
    check(wpa_config_parse_fullmac_allow_diagnostic(NULL, &ssid, 1, NULL) < 0,
          "diagnostic parser rejects null");
    check(ssid.fullmac_allow_diagnostic == 1,
          "invalid diagnostic value leaves prior setting intact");
    value = wpa_config_write_fullmac_allow_diagnostic(&no_offset, &ssid);
    check(value != NULL && strcmp(value, "1") == 0,
          "diagnostic writer retains direct field value");
    os_free(value);

    check(wpa_config_parse_key_mgmt(NULL, &ssid, 1, "SAE") == 0,
          "parse key_mgmt=SAE");
    check(ssid.key_mgmt == WPA_KEY_MGMT_SAE,
          "SAE key management stays distinct from WPA-PSK");
    value = wpa_config_write_key_mgmt(NULL, &ssid);
    check(value != NULL && strcmp(value, "SAE") == 0,
          "serialize key_mgmt=SAE");
    os_free(value);
    check(wpa_config_parse_key_mgmt(NULL, &ssid, 1, "SAE WPA-PSK") == 0,
          "parse explicit mixed AKMs for later profile rejection");
    check(ssid.key_mgmt == (WPA_KEY_MGMT_SAE | WPA_KEY_MGMT_PSK),
          "mixed AKM parsed without collapsing SAE into PSK");
}

static void test_password_parsing_and_wiping(void)
{
    struct wpa_ssid ssid;
    char quoted[68];
    size_t i;
    u8 *old;

    memset(&ssid, 0, sizeof(ssid));
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "\"A\"") == 0,
          "accept minimum one-byte quoted password");
    check(ssid.sae_password_len == 1 && ssid.sae_password[0] == 'A',
          "one-byte password length is explicit");
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "616263") == 0,
          "accept hexadecimal password via shared decoder");
    check(ssid.sae_password_len == 3 && memcmp(ssid.sae_password, "abc", 3) == 0,
          "hexadecimal password decoded exactly");

    old = ssid.sae_password;
    watch_memory(old, ssid.sae_password_len);
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "\"replacement\"") == 0,
          "replace an owned SAE password");
    check(watch_observed && watch_all_zero,
          "old owned password including terminator is wiped before free");

    memset(quoted, 'B', 63);
    quoted[0] = '"';
    for (i = 1; i <= 63; i++)
        quoted[i] = 'B';
    quoted[64] = '"';
    quoted[65] = '\0';
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, quoted) == 0,
          "accept maximum 63-byte password");
    check(ssid.sae_password_len == 63, "63-byte length retained");
    quoted[64] = 'B';
    quoted[65] = '"';
    quoted[66] = '\0';
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, quoted) < 0,
          "reject 64-byte password");
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "\"\"") < 0,
          "reject empty quoted password");
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "abc") < 0,
          "reject odd-length hexadecimal password");
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "01xz") < 0,
          "reject malformed hexadecimal password");
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, NULL) < 0,
          "reject null password input");

    watch_next_decoded_secret = 1;
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "410042") < 0,
          "reject embedded NUL in decoded SAE password");
    check(last_decoded != NULL && watch_observed && watch_all_zero,
          "rejected decoded secret is wiped before free");

    watch_memory(ssid.sae_password, ssid.sae_password_len);
    check(wpa_config_parse_sae_password(NULL, &ssid, 1, "NULL") == 0,
          "NULL clears SAE password");
    check(watch_observed && watch_all_zero && ssid.sae_password == NULL &&
          ssid.sae_password_len == 0,
          "clear wipes owned SAE password and resets pointer and length");
}

static void test_validation_and_pbkdf2_separation(void)
{
    struct wpa_ssid ssid;
    unsigned int before;
    int key, proto, pairwise, group, mode;

    profile_sae(&ssid);
    before = pbkdf2_calls;
    check(wpa_config_validate_network(&ssid, 1) == 0,
          "pure SAE parses without CONFIG_IEEE80211W and is valid for typed offload");
    check(pbkdf2_calls == before,
          "SAE validation never derives a WPA2 PBKDF2 PSK");

    key = ssid.key_mgmt; proto = ssid.proto; pairwise = ssid.pairwise_cipher;
    group = ssid.group_cipher; mode = ssid.mode;
    ssid.key_mgmt |= WPA_KEY_MGMT_PSK;
    before = pbkdf2_calls;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject mixed SAE and PSK key management");
    check(pbkdf2_calls == before, "mixed SAE config still skips PBKDF2");
    ssid.key_mgmt = key;

    ssid.passphrase = (char *) "legacy WPA2 passphrase";
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject WPA2 passphrase on SAE profile");
    ssid.passphrase = NULL;
    ssid.psk_set = 1;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject a preconfigured PSK on SAE profile");
    ssid.psk_set = 0;

    ssid.sae_password_len = 0;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject zero-length SAE password in profile validation");
    ssid.sae_password_len = 64;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject overlength SAE password in profile validation");
    ssid.sae_password_len = 12;
    ssid.proto = proto | WPA_PROTO_WPA;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject WPA plus RSN transition profile");
    ssid.proto = proto;
    ssid.pairwise_cipher = pairwise | WPA_CIPHER_TKIP;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject pairwise cipher set other than CCMP only");
    ssid.pairwise_cipher = pairwise;
    ssid.group_cipher = group | WPA_CIPHER_TKIP;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject group cipher set other than CCMP only");
    ssid.group_cipher = group;
    ssid.mode = WPAS_MODE_IBSS;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject non-managed SAE profile");
    ssid.mode = mode;
    ssid.wep_key_len[0] = 5;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "reject SAE profile containing WEP key material");
    ssid.wep_key_len[0] = 0;

    ssid.key_mgmt = WPA_KEY_MGMT_PSK;
    check(wpa_config_validate_network(&ssid, 1) != 0,
          "refuse sae_password on non-SAE profile");
    check(pbkdf2_calls == before,
          "non-SAE profile without WPA2 passphrase does not derive a key");
    wpa_config_clear_sae_password(&ssid);
    ssid.passphrase = (char *) "accepted WPA2 passphrase";
    ssid.psk_set = 0;
    before = pbkdf2_calls;
    check(wpa_config_validate_network(&ssid, 1) == 0,
          "retain legacy WPA2 passphrase validation");
    check(pbkdf2_calls == before + 1,
          "legacy WPA2 passphrase still reaches PBKDF2 derivation");
    ssid.passphrase = NULL;

    check(strstr(captured_logs, "NeverLogSecret") == NULL,
          "captured production logs contain no SAE credential");
}

static void test_persistence_writer_and_free(void)
{
    struct wpa_ssid *ssid = os_zalloc(sizeof(*ssid));
    FILE *f;
    char *output;
    char *key_value;

    check(ssid != NULL, "allocate network for config writer/free");
    wpa_config_set_network_defaults(ssid);
    ssid->key_mgmt = WPA_KEY_MGMT_SAE;
    check(wpa_config_parse_sae_password(NULL, ssid, 1,
                                        "\"NeverLogSecret\"") == 0,
          "set password for persistence check");
    f = tmpfile();
    check(f != NULL, "open temporary config stream");
    watch_ptr = NULL;
    watch_enabled = 0;
    write_sae_password(f, ssid);
    check(watch_observed && watch_all_zero,
          "serialized SAE secret is wiped after config persistence");
    write_key_mgmt(f, ssid);
    write_int(f, "fullmac_allow_diagnostic", 1, 0);
    output = read_stream(f);
    check(strcmp(output,
                 "\tsae_password=\"NeverLogSecret\"\n"
                 "\tkey_mgmt=SAE\n"
                 "\tfullmac_allow_diagnostic=1\n") == 0,
          "config writer persists the separate SAE password and diagnostic setting");
    check(strstr(captured_logs, "NeverLogSecret") == NULL,
          "persistence does not leak SAE credential to logs");
    free(output);
    fclose(f);

    key_value = wpa_config_write_sae_password(NULL, ssid);
    check(key_value != NULL && strcmp(key_value, "\"NeverLogSecret\"") == 0,
          "production SAE table writer uses shared quoted/hex format");
    memset(key_value, 0, strlen(key_value));
    os_free(key_value);
    watch_memory(ssid->sae_password, ssid->sae_password_len);
    wpa_config_free_ssid(ssid);
    check(watch_observed && watch_all_zero,
          "network free wipes the owned SAE password before release");
}

int main(void)
{
    test_parser_writer_and_key_mgmt();
    test_password_parsing_and_wiping();
    test_validation_and_pbkdf2_separation();
    test_persistence_writer_and_free();
    check(os_allocations == os_releases,
          "bounded mock allocator has no unfreed production allocations");
    puts("SAE configuration tests passed");
    return 0;
}
'''

    # Record the rendered password copy before the production config writer
    # frees it; the helper must scrub that temporary serialization.
    harness = harness.replace(
        '    if (strcmp(field, "sae_password") == 0)\n'
        '        return wpa_config_write_sae_password(NULL, ssid);',
        '    if (strcmp(field, "sae_password") == 0) {\n'
        '        char *value = wpa_config_write_sae_password(NULL, ssid);\n'
        '        if (value != NULL) watch_memory((u8 *) value, strlen(value));\n'
        '        return value;\n'
        '    }',
    )

    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise SystemExit("C compiler not found (set CC to a supported compiler)")

    with tempfile.TemporaryDirectory(prefix="wireless-sae-config-") as temp:
        source_path = Path(temp) / "sae_config_test.c"
        source_path.write_text(HARNESS_PREFIX + extracted + "\n" + harness)
        for optimization in ("-O0", "-O2"):
            binary = Path(temp) / f"sae-config-{optimization[2:]}"
            command = cc + [
                "-std=gnu99",
                optimization,
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-Wno-unused-parameter",
                "-I",
                str(WM / "src"),
                str(source_path),
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True)
            subprocess.run(
                [str(binary)],
                check=True,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"},
            )

    print("Production SAE config parser/validator/writer/free checks passed at O0 and O2.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
