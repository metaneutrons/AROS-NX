#!/usr/bin/env python3
"""Exercise the actual reference helper's two independent error channels."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


source = (Path(__file__).parent / "idf-sdio-reference/main/main.c").read_text()
start = source.index("static esp_err_t send_command(")
end = source.index("static void idle_forever(", start)
helper = source[start:end]
prefix = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define SDIO_SLOT 1
#define SDIO_CMD_TIMEOUT_MS 500U
typedef struct {
    uint32_t opcode, arg;
    int flags;
    unsigned timeout_ms;
    esp_err_t error;
    uint32_t response[4];
} sdmmc_command_t;
static esp_err_t api_result, peripheral_result;
static unsigned calls;
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
static esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t *c) {
    assert(slot == SDIO_SLOT);
    assert(c->opcode == 5 && c->arg == 0 && c->flags == 0x1030);
    assert(c->timeout_ms == SDIO_CMD_TIMEOUT_MS);
    ++calls;
    c->error = peripheral_result;
    c->response[0] = UINT32_C(0x81200000);
    return api_result;
}
'''
suffix = r'''
int main(void) {
    const esp_err_t errors[] = {0, 0x107, 0x109, -1};
    unsigned checks = 0;
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            sdmmc_command_t c = {0};
            api_result = errors[i];
            peripheral_result = errors[j];
            esp_err_t expected = api_result ? api_result : peripheral_result;
            assert(send_command("fixture", 5, 0, 0x1030, &c) == expected);
            assert(c.error == peripheral_result);
            assert(c.response[0] == UINT32_C(0x81200000));
            ++checks;
        }
    }
    assert(calls == 16 && checks == 16);
    puts("PASS actual helper: 16 API/peripheral result combinations");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="idf-sdio-result-") as tmp:
    cfile = Path(tmp) / "fixture.c"
    cfile.write_text(prefix + helper + suffix)
    for optimization in ("-O0", "-O2"):
        binary = Path(tmp) / optimization[1:]
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-Wconversion", "-Wsign-conversion", optimization,
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
