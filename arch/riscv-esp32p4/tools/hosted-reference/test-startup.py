#!/usr/bin/env python3
"""Compile focused host tests from the pinned ESP-Hosted transport source."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


FUNCTION_SIGNATURES = (
    "uint8_t is_transport_tx_ready(void)",
    "static void reset_slave(void)",
    "esp_err_t transport_drv_reconfigure(void)",
)


def extract_function(source: str, signature: str, path: Path) -> tuple[int, str]:
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"{signature} not found in {path}")

    opening = source.find("{", start + len(signature))
    if opening < 0:
        raise ValueError(f"body for {signature} not found in {path}")

    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(source):
        char = source[i]
        following = source[i + 1] if i + 1 < len(source) else ""

        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                i += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line-comment"
            i += 1
        elif char == "/" and following == "*":
            state = "block-comment"
            i += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return start, source[start : i + 1]
        i += 1

    raise ValueError(f"unterminated body for {signature} in {path}")


def extract_functions(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    found = [extract_function(source, signature, path) for signature in FUNCTION_SIGNATURES]
    found.sort(key=lambda item: item[0])
    return "\n\n".join(function for _, function in found)


def make_test_source(functions: str) -> str:
    return f'''#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
typedef struct {{
    void (*_h_config_gpio)(int port, int pin, int mode);
    void (*_h_write_gpio)(int port, int pin, int value);
    void (*_h_msleep)(uint32_t milliseconds);
    void (*_h_sleep)(uint32_t seconds);
}} hosted_functions_t;

#define CONFIG_ESP_GPIO_SLAVE_RESET_SLAVE (-1)
#define ESP_OK ((esp_err_t)0)
#define ESP_ERR_TIMEOUT ((esp_err_t)0x107)
#define TRANSPORT_INACTIVE 0
#define TRANSPORT_RX_ACTIVE 1
#define TRANSPORT_TX_ACTIVE 2
#define TAG "transport"
#define ESP_LOGI(tag, ...) do {{ (void)(tag); }} while (0)
#define ESP_LOGE(tag, ...) do {{ (void)(tag); }} while (0)

static uint8_t transport_state = TRANSPORT_INACTIVE;
static hosted_functions_t hosted_functions;
static struct {{ hosted_functions_t *funcs; }} g_h;
static int64_t mock_time_us;
static unsigned gpio_config_calls;
static unsigned gpio_write_calls;
static unsigned hosted_msleep_calls;
static unsigned hosted_sleep_calls;
static unsigned invalid_msleep_delays;
static unsigned ready_after_msleep;

int64_t esp_timer_get_time(void);

{functions}

static unsigned checks;
static unsigned cases;
static unsigned failures;

static void check(int condition, const char *label)
{{
    ++checks;
    if (!condition) {{
        ++failures;
        fprintf(stderr, "FAIL: %s\\n", label);
    }}
}}

static void mock_config_gpio(int port, int pin, int mode)
{{
    (void)port;
    (void)pin;
    (void)mode;
    ++gpio_config_calls;
}}

static void mock_write_gpio(int port, int pin, int value)
{{
    (void)port;
    (void)pin;
    (void)value;
    ++gpio_write_calls;
}}

static void mock_msleep(uint32_t milliseconds)
{{
    ++hosted_msleep_calls;
    if (milliseconds != 20)
        ++invalid_msleep_delays;
    mock_time_us += (int64_t)milliseconds * INT64_C(1000);
    if (ready_after_msleep != 0 && hosted_msleep_calls == ready_after_msleep)
        transport_state = TRANSPORT_TX_ACTIVE;
}}

static void mock_sleep(uint32_t seconds)
{{
    (void)seconds;
    ++hosted_sleep_calls;
}}

int64_t esp_timer_get_time(void)
{{
    return mock_time_us;
}}

static void reset_mocks(uint8_t initial_state, unsigned ready_after)
{{
    mock_time_us = 0;
    gpio_config_calls = 0;
    gpio_write_calls = 0;
    hosted_msleep_calls = 0;
    hosted_sleep_calls = 0;
    invalid_msleep_delays = 0;
    ready_after_msleep = ready_after;
    transport_state = initial_state;
    hosted_functions._h_config_gpio = mock_config_gpio;
    hosted_functions._h_write_gpio = mock_write_gpio;
    hosted_functions._h_msleep = mock_msleep;
    hosted_functions._h_sleep = mock_sleep;
    g_h.funcs = &hosted_functions;
}}

static void check_no_gpio_access(void)
{{
    check(gpio_config_calls == 0, "negative reset GPIO performs no GPIO configuration");
    check(gpio_write_calls == 0, "negative reset GPIO performs no GPIO writes");
    check(hosted_sleep_calls == 0, "negative reset GPIO performs no reset sleep");
}}

static void test_negative_gpio_path(void)
{{
    esp_err_t result;
    ++cases;
    reset_mocks(TRANSPORT_INACTIVE, 1);
    result = transport_drv_reconfigure();
    check(result == ESP_OK, "negative GPIO path can reach TX readiness");
    check(hosted_msleep_calls == 1, "negative GPIO path makes one readiness wait");
    check(mock_time_us == INT64_C(20000), "readiness wait advances mocked time by 20 ms");
    check(transport_state == TRANSPORT_TX_ACTIVE, "readiness wait publishes TX state");
    check(invalid_msleep_delays == 0, "readiness wait uses the source 20 ms interval");
    check_no_gpio_access();
}}

static void test_already_ready(void)
{{
    esp_err_t result;
    ++cases;
    reset_mocks(TRANSPORT_TX_ACTIVE, 1);
    result = transport_drv_reconfigure();
    check(result == ESP_OK, "already-ready transport succeeds immediately");
    check(hosted_msleep_calls == 0, "already-ready transport does not wait");
    check(mock_time_us == 0, "already-ready transport does not advance mocked time");
    check(transport_state == TRANSPORT_TX_ACTIVE, "already-ready state is preserved");
    check_no_gpio_access();
}}

static void test_never_ready_times_out(void)
{{
    esp_err_t result;
    ++cases;
    reset_mocks(TRANSPORT_INACTIVE, 0);
    result = transport_drv_reconfigure();
    check(result == ESP_ERR_TIMEOUT, "never-ready transport times out");
    check(hosted_msleep_calls == 500, "timeout performs 500 bounded 20 ms waits");
    check(mock_time_us == INT64_C(10000000), "timeout occurs at the 10 second deadline");
    check(transport_state == TRANSPORT_RX_ACTIVE, "timeout leaves RX active");
    check(invalid_msleep_delays == 0, "timeout uses only 20 ms readiness waits");
    check_no_gpio_access();
}}

static void test_ready_before_deadline_succeeds(void)
{{
    esp_err_t result;
    ++cases;
    reset_mocks(TRANSPORT_INACTIVE, 499);
    result = transport_drv_reconfigure();
    check(result == ESP_OK, "readiness before deadline succeeds");
    check(hosted_msleep_calls == 499, "readiness is observed on the 499th wait");
    check(mock_time_us == INT64_C(9980000), "readiness is observed 20 ms before deadline");
    check(transport_state == TRANSPORT_TX_ACTIVE, "pre-deadline readiness publishes TX state");
    check(invalid_msleep_delays == 0, "pre-deadline path uses 20 ms readiness waits");
    check_no_gpio_access();
}}

static void test_ready_at_deadline_is_rejected(void)
{{
    esp_err_t result;
    ++cases;
    reset_mocks(TRANSPORT_INACTIVE, 500);
    result = transport_drv_reconfigure();
    check(result == ESP_ERR_TIMEOUT, "readiness first observed at deadline is rejected");
    check(hosted_msleep_calls == 500, "deadline case performs 500 readiness waits");
    check(mock_time_us == INT64_C(10000000), "deadline case reaches exactly 10 seconds");
    check(transport_state == TRANSPORT_TX_ACTIVE,
          "readiness became true at the deadline before timeout was returned");
    check_no_gpio_access();
}}

static void test_repeated_calls_use_independent_deadlines(void)
{{
    esp_err_t first_result;
    esp_err_t second_result;
    unsigned first_wait_count;
    int64_t first_end_time;
    ++cases;
    reset_mocks(TRANSPORT_INACTIVE, 0);
    first_result = transport_drv_reconfigure();
    first_wait_count = hosted_msleep_calls;
    first_end_time = mock_time_us;
    check(first_result == ESP_ERR_TIMEOUT, "first repeated call times out");
    check(first_wait_count == 500, "first call consumes its full timeout budget");
    check(transport_state == TRANSPORT_RX_ACTIVE, "first timeout leaves TX not ready");

    ready_after_msleep = first_wait_count + 2;
    second_result = transport_drv_reconfigure();
    check(second_result == ESP_OK, "second call gets a fresh deadline and succeeds");
    check(hosted_msleep_calls - first_wait_count == 2,
          "second call waits independently for its scheduled readiness");
    check(mock_time_us - first_end_time == INT64_C(40000),
          "second call advances time only by its two waits");
    check(transport_state == TRANSPORT_TX_ACTIVE, "second call publishes TX readiness");
    check(invalid_msleep_delays == 0, "repeated calls use 20 ms readiness waits");
    check_no_gpio_access();
}}

int main(void)
{{
    test_negative_gpio_path();
    test_already_ready();
    test_never_ready_times_out();
    test_ready_before_deadline_succeeds();
    test_ready_at_deadline_is_rejected();
    test_repeated_calls_use_independent_deadlines();

    if (failures != 0) {{
        fprintf(stderr, "FAIL: %u of %u checks failed across %u cases\\n",
                failures, checks, cases);
        return 1;
    }}
    printf("PASS: %u cases, %u checks\\n", cases, checks);
    return 0;
}}
'''


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile focused host tests from ESP-Hosted transport_drv.c"
    )
    parser.add_argument(
        "source",
        type=Path,
        help="path to pinned host/drivers/transport/transport_drv.c",
    )
    args = parser.parse_args()

    compiler_words = shlex.split(os.environ.get("CC", "cc"))
    if not compiler_words:
        parser.error("CC must name a C compiler")
    compiler = shutil.which(compiler_words[0])
    if compiler is None:
        parser.error(f"C compiler not found: {compiler_words[0]}")
    compiler_words[0] = compiler

    try:
        functions = extract_functions(args.source)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="hosted-startup-test-") as tmp:
        source_path = Path(tmp) / "test_startup.c"
        source_path.write_text(make_test_source(functions), encoding="utf-8")

        for optimization in ("-O0", "-O2"):
            binary_path = Path(tmp) / f"test_startup_{optimization[2:]}"
            command = [
                *compiler_words,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                optimization,
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary_path),
            ]
            print(f"{optimization}: compiling extracted transport functions with ASan and UBSan")
            built = subprocess.run(command, text=True, capture_output=True)
            if built.stdout:
                print(built.stdout, end="")
            if built.stderr:
                print(built.stderr, end="", file=sys.stderr)
            if built.returncode != 0:
                print(f"{optimization}: compiler exited {built.returncode}", file=sys.stderr)
                return built.returncode

            run = subprocess.run([str(binary_path)], text=True, capture_output=True)
            if run.stdout:
                print(f"{optimization}: {run.stdout}", end="")
            if run.stderr:
                print(run.stderr, end="", file=sys.stderr)
            if run.returncode != 0:
                print(f"{optimization}: test executable exited {run.returncode}",
                      file=sys.stderr)
                return run.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
