#!/usr/bin/env python3
"""Exercise the actual startup policy builder, not hardware preparation.

The called service is a capture mock. Canonical aperture/L2 validation belongs
to the independent soft-atomic fixture; this checks ownership boundaries and
the bootstrap call ordering before the first Exec lock can be initialized.
"""
from pathlib import Path
import subprocess
import tempfile

kernel = Path(__file__).resolve().parent.parent
source = (kernel / "kernel_startup.c").read_text()
start = source.index("static int krnP4PrepareRuntimeAtomics(")
end = source.index("\n#endif", start)
body = source[start:end]
# The header's contents are real host data; only its physical address is
# transported by a fixture variable so no fixed low mapping is needed.
body = body.replace("(uintptr_t)__esp32p4_mh_psram;",
                    "test_external_header_address;")
bootstrap = source[source.index("static void krnStartExec(void)"):]
assert bootstrap.index("krnP4PrepareRuntimeAtomics(mh)") < bootstrap.index(
    "if (!krnPrepareExecBase(")
assert bootstrap.index('"csrrci %0, mstatus, 8"') < bootstrap.index(
    "krnP4PrepareRuntimeAtomics(mh)")
fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "p4_soft_atomic.h"
struct MemHeader { void *mh_Upper; };
#define __data_start ((char *)(uintptr_t)0x4ff08000u)
#define __kernel_end ((char *)(uintptr_t)0x4ff20000u)
#define P4_HEAP_LOW_END 0x4ff3ffc0u
#define P4_PSRAM_WINDOW_BASE 0x48000000u
#define P4_FB_BASE 0x49b00000u
static struct MemHeader *__esp32p4_mh_high, *__esp32p4_mh_psram;
static uint32_t __esp32p4_psram_size = 0x2000000u;
static uintptr_t p4_modules_owned_begin;
static uintptr_t test_external_header_address;
static void *__esp32p4_modules_high;
static struct p4_soft_atomic_policy captured;
static enum p4_soft_atomic_result result = P4_SOFT_ATOMIC_OK;
static unsigned int calls, checks;
#define CHECK(x) do { checks++; if (!(x)) { \
 fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static enum p4_soft_atomic_result krnP4SoftAtomicPrepare(
    const struct p4_soft_atomic_policy *policy)
{ captured = *policy; calls++; return result; }
'''
# Avoid clashing with the API's external declaration in the fixture.
fixture = fixture.replace("static enum p4_soft_atomic_result krnP4SoftAtomicPrepare",
                          "enum p4_soft_atomic_result krnP4SoftAtomicPrepare")
main = r'''
int main(void)
{
 struct MemHeader low = { (void *)(uintptr_t)P4_HEAP_LOW_END };
 struct MemHeader high = { (void *)(uintptr_t)0x4ffa0000u };
 /* These headers are host objects; the real service validates addresses. */
 struct MemHeader external = { (void *)(uintptr_t)P4_FB_BASE };
 CHECK(krnP4PrepareRuntimeAtomics(&low));
 CHECK(captured.region_count == 1);
 CHECK(captured.regions[0].begin == (uintptr_t)__data_start);
 CHECK(captured.regions[0].end == P4_HEAP_LOW_END);
 CHECK(captured.regions[0].kind == P4_SOFT_ATOMIC_REGION_SRAM);
 CHECK(captured.max_attempts == 20000u && captured.max_cycles == 1000000u);
 __esp32p4_mh_high = &high;
 CHECK(krnP4PrepareRuntimeAtomics(&low));
 CHECK(captured.region_count == 2);
 CHECK(captured.regions[1].begin == (uintptr_t)&high);
 CHECK(captured.regions[1].end == 0x4ffa0000u);
 CHECK(captured.regions[1].kind == P4_SOFT_ATOMIC_REGION_SRAM_HIGH);
 result = P4_SOFT_ATOMIC_BAD_POLICY;
 CHECK(!krnP4PrepareRuntimeAtomics(&low));
 result = P4_SOFT_ATOMIC_OK;
 unsigned before = calls;
 low.mh_Upper = (void *)(uintptr_t)(P4_HEAP_LOW_END + 1u);
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 low.mh_Upper = __kernel_end;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 low.mh_Upper = (void *)(uintptr_t)P4_HEAP_LOW_END;
 p4_modules_owned_begin = 0x48100000u;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 p4_modules_owned_begin = 0;
 /* A nonsensical/out-of-aperture heap is rejected before publication. */
 __esp32p4_mh_psram = &external;
 test_external_header_address = 0x48100000u;
 CHECK(krnP4PrepareRuntimeAtomics(&low));
 CHECK(captured.region_count == 3);
 CHECK(captured.regions[2].begin == 0x48100000u);
 CHECK(captured.regions[2].end == P4_FB_BASE);
 CHECK(captured.regions[2].kind == P4_SOFT_ATOMIC_REGION_PSRAM);
 test_external_header_address = 0x48400000u;
 p4_modules_owned_begin = 0x48300000u;
 __esp32p4_modules_high = (void *)(uintptr_t)0x48380000u;
 CHECK(krnP4PrepareRuntimeAtomics(&low));
 CHECK(captured.regions[2].begin == 0x48300000u);
 CHECK(captured.regions[2].end == P4_FB_BASE);
 before = calls;
 __esp32p4_modules_high = (void *)(uintptr_t)0x48400001u;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 __esp32p4_modules_high = (void *)(uintptr_t)0x48380000u;
 p4_modules_owned_begin = 0x48400000u;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 p4_modules_owned_begin = 0;
 test_external_header_address = 0x47ffffffu;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 test_external_header_address = 0x48400000u;
 external.mh_Upper = (void *)(uintptr_t)(P4_FB_BASE + 1u);
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 external.mh_Upper = (void *)(uintptr_t)P4_FB_BASE;
 __esp32p4_psram_size = 0x1000000u;
 CHECK(!krnP4PrepareRuntimeAtomics(&low)); CHECK(calls == before);
 printf("PASS: %u runtime atomic-policy ownership checks\n", checks);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-runtime-policy-") as tmp:
    src = Path(tmp) / "policy.c"
    binary = Path(tmp) / "policy-test"
    src.write_text(fixture + body + main)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(kernel),
                    str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
