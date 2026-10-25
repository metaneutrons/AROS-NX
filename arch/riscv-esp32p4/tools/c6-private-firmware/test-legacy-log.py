#!/usr/bin/env python3
"""Test actual C6 legacy helper bodies; not firmware/UART certification."""
import pathlib
import re
import subprocess
import sys
import tempfile

idf = pathlib.Path(sys.argv[1])
for component in ("esp_wifi", "esp_coex", "esp_phy"):
    source = (idf / "components" / component / "src/lib_printf.c").read_text()
    start = source.index("static int lib_printf(")
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    helper = source[start:end]
    fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define CONFIG_IDF_TARGET_ESP32C6 1
#define VPRINTF_STACK_BUFFER_SIZE 80
static unsigned sink_calls;
#define ESP_LOGI(tag, ...) do { (void)(tag); ++sink_calls; } while (0)
'''
    fixture += helper
    fixture += r'''
static void exercise(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = lib_printf("fixture", format, args);
    va_end(args);
    assert(result == 0);
    assert(sink_calls == 0);
}
int main(void)
{
    exercise("SSID=%s key=%s", "fake-network", "fake-key");
    exercise("%s %n"); /* Missing arguments must not be interpreted. */
    exercise(NULL);
    puts("legacy C6 helper: 3 cases/6 checks PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="c6-legacy-log-") as directory:
        root = pathlib.Path(directory)
        test = root / "test.c"
        test.write_text(fixture)
        for optimization in ("-O0", "-O2"):
            executable = root / ("test" + optimization)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            optimization, "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(test), "-o",
                            str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
    # Prevent a future direct stdio/ROM replacement from passing the sink stub.
    body = helper.split("#if CONFIG_IDF_TARGET_ESP32C6\n", 1)[1].split("#else", 1)[0]
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
    if re.sub(r"\s+", "", body) != "(void)tag;(void)format;(void)arg;return0;":
        raise RuntimeError("unexpected C6 diagnostic body: " + component)
    print(component + ": source guard PASS", flush=True)
