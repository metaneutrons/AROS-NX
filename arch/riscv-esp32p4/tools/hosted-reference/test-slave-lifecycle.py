#!/usr/bin/env python3
"""Test the pinned slave Wi-Fi handler lifecycle using extracted source."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tempfile


EXTRACTOR = Path(__file__).resolve().with_name("test-startup.py")
MACRO_NAMES = ("RPC_TEMPLATE", "RPC_TEMPLATE_SIMPLE", "RPC_RET_FAIL_IF")
SIGNATURES = (
    "static esp_err_t ensure_wifi_event_handler(void)",
    "static esp_err_t req_wifi_init(Rpc *req, Rpc *resp, void *priv_data)",
    "static esp_err_t req_wifi_deinit(Rpc *req, Rpc *resp, void *priv_data)",
)
DECLARATIONS = (
    "static esp_event_handler_instance_t instance_any_id;",
    "static uint8_t wifi_event_handler_registered;",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_source(path: Path) -> tuple[str, list[str]]:
    source = path.read_text(encoding="utf-8")
    namespace = runpy.run_path(str(EXTRACTOR))
    extract_function = namespace["extract_function"]
    functions = []
    for signature in SIGNATURES:
        if source.count(signature) != 1:
            raise ValueError(f"expected exactly one {signature!r} in {path}")
        _, function = extract_function(source, signature, path)
        functions.append(function)

    declarations = []
    for declaration in DECLARATIONS:
        if source.count(declaration) != 1:
            raise ValueError(f"expected exactly one {declaration!r} in {path}")
        declarations.append(declaration)

    lines = source.splitlines()
    macros = []
    for name in MACRO_NAMES:
        prefix = f"#define {name}("
        matches = [index for index, line in enumerate(lines)
                   if line.lstrip().startswith(prefix)]
        if len(matches) != 1:
            raise ValueError(f"expected exactly one {prefix!r} macro in {path}")
        index = matches[0]
        macro_lines = [lines[index]]
        while macro_lines[-1].rstrip().endswith(chr(92)):
            index += 1
            if index == len(lines):
                raise ValueError(f"unterminated {name} macro in {path}")
            macro_lines.append(lines[index])
        macros.append("\n".join(macro_lines))

    init_function = functions[1]
    register_call = "RPC_RET_FAIL_IF(ensure_wifi_event_handler());"
    init_call = "RPC_RET_FAIL_IF(esp_wifi_init(&cfg));"
    if register_call not in init_function or init_call not in init_function:
        raise ValueError("req_wifi_init does not use the expected lifecycle calls")
    if init_function.index(register_call) > init_function.index(init_call):
        raise ValueError("req_wifi_init must register before esp_wifi_init")
    if "esp_event_handler_instance_register" in init_function:
        raise ValueError("registration must be centralized in the helper")
    if "esp_event_handler_instance_unregister" in functions[2]:
        raise ValueError("this test expects the handler to live with default loop")
    return source, declarations + macros + functions


def make_test_source(extracted: list[str]) -> str:
    declarations = extracted[: len(DECLARATIONS)]
    macro_start = len(DECLARATIONS)
    macros = extracted[macro_start : macro_start + len(MACRO_NAMES)]
    functions = extracted[macro_start + len(MACRO_NAMES) :]
    prefix = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t esp_err_t;
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

#define ESP_OK ((esp_err_t)0)
#define ESP_FAIL ((esp_err_t)0x101)
#define ESP_ERR_NO_MEM ((esp_err_t)0x105)
#define ESP_EVENT_ANY_ID (-1)
#define WIFI_EVENT wifi_event_base
#define SUCCESS ESP_OK
#define TAG "lifecycle-test"
#define ESP_LOGE(tag, ...) ((void)(tag))
#define WIFI_INIT_CONFIG_DEFAULT() {0}

static const char wifi_event_base[] = "WIFI_EVENT";
static uint8_t registration_cookie;
static unsigned checks;
static unsigned failures;
static unsigned cases;
static unsigned sequence;
static unsigned registration_calls;
static unsigned wifi_init_calls;
static unsigned wifi_deinit_calls;
static unsigned callback_calls;
static unsigned registration_order;
static unsigned wifi_init_order;
static unsigned callback_order;
static unsigned bad_registration_arguments;
static int emit_event_during_init;
static esp_err_t registration_result;
static esp_err_t wifi_init_result;
static esp_err_t wifi_deinit_result;
static esp_event_handler_t registered_handler;
static void *registered_arg;

typedef struct {
    int static_rx_buf_num;
    int dynamic_rx_buf_num;
    int tx_buf_type;
    int static_tx_buf_num;
    int dynamic_tx_buf_num;
    int cache_tx_buf_num;
    int csi_enable;
    int ampdu_rx_enable;
    int ampdu_tx_enable;
    int amsdu_tx_enable;
    int nvs_enable;
    int nano_enable;
    int rx_ba_win;
    int wifi_task_core_id;
    int beacon_max_len;
    int mgmt_sbuf_num;
    uint64_t feature_caps;
    int sta_disconnected_pm;
    int espnow_max_encrypt_num;
    int magic;
} wifi_init_config_t;

typedef struct {
    wifi_init_config_t *cfg;
} RpcReqWifiInit;

typedef struct {
    int resp;
} RpcRespWifiInit;

typedef struct {
    int resp;
} RpcRespWifiDeinit;

typedef struct Rpc {
    RpcReqWifiInit *req_wifi_init;
    RpcRespWifiInit *resp_wifi_init;
    RpcRespWifiDeinit *resp_wifi_deinit;
} Rpc;

static void rpc__resp__wifi_init__init(RpcRespWifiInit *response)
{
    response->resp = 0;
}

static void rpc__resp__wifi_deinit__init(RpcRespWifiDeinit *response)
{
    response->resp = 0;
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)event_data;
    ++callback_calls;
    callback_order = ++sequence;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *handler_arg,
    esp_event_handler_instance_t *instance);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);

@@MACROS@@

@@DECLARATIONS@@

@@FUNCTIONS@@

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *handler_arg,
    esp_event_handler_instance_t *instance)
{
    ++registration_calls;
    registration_order = ++sequence;
    if (event_base != WIFI_EVENT || event_id != ESP_EVENT_ANY_ID ||
        handler != event_handler || handler_arg != NULL ||
        instance != &instance_any_id) {
        ++bad_registration_arguments;
    }
    if (registration_result == ESP_OK) {
        registered_handler = handler;
        registered_arg = handler_arg;
        if (instance) *instance = &registration_cookie;
    }
    return registration_result;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    ++wifi_init_calls;
    wifi_init_order = ++sequence;
    if (emit_event_during_init && registered_handler) {
        registered_handler(registered_arg, WIFI_EVENT, 0, NULL);
    }
    return wifi_init_result;
}

esp_err_t esp_wifi_deinit(void)
{
    ++wifi_deinit_calls;
    return wifi_deinit_result;
}

static void check(int condition, const char *label)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", label);
    }
}

static void reset_case(esp_err_t register_ret, esp_err_t init_ret,
                       esp_err_t deinit_ret)
{
    wifi_event_handler_registered = 0;
    instance_any_id = NULL;
    sequence = 0;
    registration_calls = 0;
    wifi_init_calls = 0;
    wifi_deinit_calls = 0;
    callback_calls = 0;
    registration_order = 0;
    wifi_init_order = 0;
    callback_order = 0;
    bad_registration_arguments = 0;
    emit_event_during_init = 1;
    registration_result = register_ret;
    wifi_init_result = init_ret;
    wifi_deinit_result = deinit_ret;
    registered_handler = NULL;
    registered_arg = NULL;
}

static int invoke_wifi_init(void)
{
    wifi_init_config_t config = {0};
    RpcReqWifiInit request_payload = {&config};
    Rpc request = {0};
    Rpc response = {0};
    int status;

    request.req_wifi_init = &request_payload;
    check(req_wifi_init(&request, &response, NULL) == ESP_OK,
          "init handler reports through its RPC response");
    check(response.resp_wifi_init != NULL, "init response is allocated");
    if (!response.resp_wifi_init) return ESP_ERR_NO_MEM;
    status = response.resp_wifi_init->resp;
    free(response.resp_wifi_init);
    return status;
}

static int invoke_wifi_deinit(void)
{
    Rpc request = {0};
    Rpc response = {0};
    int status;

    check(req_wifi_deinit(&request, &response, NULL) == ESP_OK,
          "deinit handler reports through its RPC response");
    check(response.resp_wifi_deinit != NULL, "deinit response is allocated");
    if (!response.resp_wifi_deinit) return ESP_ERR_NO_MEM;
    status = response.resp_wifi_deinit->resp;
    free(response.resp_wifi_deinit);
    return status;
}

static void test_reinit_keeps_one_registration(void)
{
    ++cases;
    reset_case(ESP_OK, ESP_OK, ESP_OK);
    check(invoke_wifi_init() == ESP_OK, "first init succeeds");
    check(registration_calls == 1 && wifi_init_calls == 1,
          "first init registers once before driver init");
    check(registration_order < wifi_init_order && callback_calls == 1 &&
          wifi_init_order < callback_order,
          "handler receives a synchronous event during driver init");
    check(instance_any_id == &registration_cookie &&
          wifi_event_handler_registered == 1,
          "successful registration stores its instance and flag");

    check(invoke_wifi_deinit() == ESP_OK, "deinit succeeds");
    check(wifi_deinit_calls == 1, "actual deinit handler called once");
    check(invoke_wifi_init() == ESP_OK, "reinit succeeds");
    check(registration_calls == 1 && wifi_init_calls == 2 &&
          callback_calls == 2,
          "successful deinit/reinit reuses one event handler");
    check(bad_registration_arguments == 0,
          "registration uses WIFI_EVENT any-ID and the saved instance");
}

static void test_registration_failure_retries_before_init(void)
{
    ++cases;
    reset_case(ESP_FAIL, ESP_OK, ESP_OK);
    check(invoke_wifi_init() == ESP_FAIL,
          "registration failure is returned in the init response");
    check(registration_calls == 1 && wifi_init_calls == 0 &&
          wifi_event_handler_registered == 0,
          "registration failure has no driver-init side effect or sticky flag");

    registration_result = ESP_OK;
    check(invoke_wifi_init() == ESP_OK, "registration retry succeeds");
    check(registration_calls == 2 && wifi_init_calls == 1 &&
          wifi_event_handler_registered == 1,
          "failed registration is retried exactly once before init");
    check(bad_registration_arguments == 0 && callback_calls == 1 &&
          registration_order < wifi_init_order &&
          wifi_init_order < callback_order,
          "retry registers the callback before a synchronous init event");
}

static void test_init_failure_does_not_duplicate_registration(void)
{
    ++cases;
    reset_case(ESP_OK, ESP_FAIL, ESP_OK);
    check(invoke_wifi_init() == ESP_FAIL, "first driver init failure propagates");
    check(registration_calls == 1 && wifi_init_calls == 1 &&
          wifi_event_handler_registered == 1,
          "successful handler registration survives init failure");

    wifi_init_result = ESP_OK;
    check(invoke_wifi_init() == ESP_OK, "driver init retry succeeds");
    check(registration_calls == 1 && wifi_init_calls == 2 &&
          callback_calls == 2,
          "init retry does not register a duplicate handler");
}

static void test_deinit_failure_does_not_clear_registration(void)
{
    ++cases;
    reset_case(ESP_OK, ESP_OK, ESP_FAIL);
    check(invoke_wifi_init() == ESP_OK, "initial driver init succeeds");
    check(invoke_wifi_deinit() == ESP_FAIL,
          "deinit failure propagates through its RPC response");
    check(invoke_wifi_init() == ESP_OK, "init after failed deinit succeeds");
    check(registration_calls == 1 && wifi_init_calls == 2 &&
          wifi_deinit_calls == 1,
          "failed deinit leaves the persistent handler registered once");
}

int main(void)
{
    test_reinit_keeps_one_registration();
    test_registration_failure_retries_before_init();
    test_init_failure_does_not_duplicate_registration();
    test_deinit_failure_does_not_clear_registration();
    if (failures) {
        fprintf(stderr, "FAIL: %u/%u checks across %u cases\n",
                failures, checks, cases);
        return 1;
    }
    printf("PASS: slave Wi-Fi handler lifecycle %u checks across %u cases\n",
           checks, cases);
    return 0;
}
'''
    return (prefix.replace("@@MACROS@@", "\n\n".join(macros))
            .replace("@@DECLARATIONS@@", "\n".join(declarations))
            .replace("@@FUNCTIONS@@", "\n\n".join(functions)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("slave_control", type=Path,
                        help="patched pinned slave/main/slave_control.c")
    arguments = parser.parse_args()
    source_path = arguments.slave_control.resolve()
    if not source_path.is_file():
        parser.error(f"source file does not exist: {source_path}")

    try:
        _, extracted = extract_source(source_path)
    except (OSError, ValueError) as error:
        print(f"source extraction failed: {error}", file=sys.stderr)
        return 2

    print(f"SHA256 {Path(__file__).resolve()} {sha256(Path(__file__).resolve())}",
          flush=True)
    print(f"SHA256 {source_path} {sha256(source_path)}", flush=True)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        print("CC resolved to an empty command", file=sys.stderr)
        return 2
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"

    with tempfile.TemporaryDirectory(prefix="hosted-slave-lifecycle-") as tmp:
        temp_dir = Path(tmp)
        c_source = temp_dir / "test_slave_lifecycle.c"
        c_source.write_text(make_test_source(extracted), encoding="utf-8")
        for optimization in ("-O0", "-O2"):
            binary = temp_dir / f"test_slave_lifecycle_{optimization[2:]}"
            command = [
                *compiler,
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-pedantic",
                optimization, "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", str(c_source), "-o", str(binary),
            ]
            print(f"RUN {optimization}: {shlex.join(command)}", flush=True)
            built = subprocess.run(command, text=True, capture_output=True,
                                   env=environment, timeout=120)
            if built.stdout:
                print(built.stdout, end="")
            if built.stderr:
                print(built.stderr, end="", file=sys.stderr)
            if built.returncode:
                return built.returncode
            run = subprocess.run([str(binary)], text=True, capture_output=True,
                                 env=environment, timeout=60)
            if run.stdout:
                print(run.stdout, end="")
            if run.stderr:
                print(run.stderr, end="", file=sys.stderr)
            if run.returncode:
                return run.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
