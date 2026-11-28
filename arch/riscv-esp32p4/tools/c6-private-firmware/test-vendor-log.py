#!/usr/bin/env python3
"""Exercise exact patched IDF callback bodies, not the vendor library/radio."""
import pathlib
import re
import subprocess
import sys
import tempfile


def extract(source, name):
    start = source.index("static void " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


source = (pathlib.Path(sys.argv[1]) /
          "components/esp_wifi/esp32c6/esp_adapter.c").read_text()
fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
typedef int esp_log_level_t;
static unsigned sink_calls;
/* A baseline callback must fail even if it only forwards, without formatting. */
void esp_log_writev(esp_log_level_t level, const char *tag,
                    const char *format, va_list args)
{
    (void)level; (void)tag; (void)format; (void)args;
    ++sink_calls;
}
'''
fixture += extract(source, "esp_log_writev_wrapper") + "\n"
fixture += extract(source, "esp_log_write_wrapper") + "\n"
fixture += r'''
static void exercise_v(unsigned level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev_wrapper(level, tag, format, args);
    va_end(args);
}
int main(void)
{
    unsigned checks = 0;
    for (unsigned level = 0; level <= 5; ++level) {
        esp_log_write_wrapper(level, "wifi", "SSID=%s key=%s", "fixture", "fake-key");
        exercise_v(level, "wifi", "SSID=%s key=%s", "fixture", "fake-key");
        assert(sink_calls == 0); ++checks;
        /* Format requiring nonexistent arguments must never be evaluated. */
        esp_log_write_wrapper(level, NULL, "%s %n");
        exercise_v(level, NULL, "%s %n");
        assert(sink_calls == 0); ++checks;
        esp_log_write_wrapper(level, NULL, NULL);
        exercise_v(level, NULL, NULL);
        assert(sink_calls == 0); ++checks;
    }
    printf("vendor log callback checks=%u PASS\n", checks);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="c6-vendor-log-") as directory:
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

# The sink recorder alone would miss a future printf/ROM-output replacement.
# Require the reviewed no-op body and unchanged callback table bindings too.
for name, arguments in (("esp_log_write_wrapper", ("level", "tag", "format")),
                        ("esp_log_writev_wrapper", ("level", "tag", "format", "args"))):
    body = extract(source, name).split("{", 1)[1][:-1]
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
    compact = re.sub(r"\s+", "", body)
    expected = "".join("(void)" + argument + ";" for argument in arguments)
    if compact != expected:
        raise RuntimeError("callback is not the reviewed no-op: " + name)
for field, wrapper in (("_log_write", "esp_log_write_wrapper"),
                       ("_log_writev", "esp_log_writev_wrapper")):
    if not re.search(r"\." + field + r"\s*=\s*" + wrapper + r"\s*,", source):
        raise RuntimeError("missing callback binding: " + field)
print("callback table/source guards PASS")
