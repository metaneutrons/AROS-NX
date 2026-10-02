#!/usr/bin/env python3
"""Extract the actual P4 TaskLaunch observer path; its primitive boundaries are mocked."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef void *APTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct Task { unsigned unused; };
struct Hook { unsigned unused; };
struct taskresource_cleanup_binding {
    uintptr_t library;
    unsigned contract;
    int (*cleanup)(struct Task *, APTR);
    BOOL (*prelaunch)(struct Task *, APTR);
};
struct ExecBase { void *lb_TaskResBase; } base;
static struct ExecBase *SysBase = &base;
static struct Task target;
static struct Hook hook;
static struct taskresource_cleanup_binding resource;
static unsigned checks, events[8], count, fatals;
static int ready_result;
static int observe_result;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static BOOL observe(struct Task *task, APTR data) {
    CHECK(task == &target && data == &resource);
    CHECK(count < 8); events[count++] = 1;
    return observe_result;
}
static void user_hook(struct Hook *h, struct Task *task, unsigned arg) {
    CHECK(h == &hook && task == &target && arg == 0);
    CHECK(count < 8); events[count++] = 2;
}
static int ready(struct Task *task) {
    CHECK(task == &target); CHECK(count < 8); events[count++] = 3;
    return ready_result;
}
#define EXEC_PLATFORM_NEW_TASK_READY(task) ready(task)
#define EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP 1
#define TASKRESOURCE_CLEANUP_CONTRACT @CONTRACT@
#define EXEC_PLATFORM_TASK_FATAL() (++fatals)
#define DADDTASK(...) ((void)0)
#define CALLHOOKPKT(h,t,a) user_hook(h,t,a)
@BODY@
static void reset(void) {
    count = fatals = 0; ready_result = observe_result = 1;
    resource.contract = TASKRESOURCE_CLEANUP_CONTRACT;
    resource.prelaunch = observe; base.lb_TaskResBase = &resource;
}
int main(void) {
    alarm(20);
    reset(); TaskLaunch(NULL, &target, &hook);
    CHECK(count == 3 && events[0] == 1 && events[1] == 2 && events[2] == 3 && fatals == 0);
    reset(); TaskLaunch(NULL, &target, NULL);
    CHECK(count == 2 && events[0] == 1 && events[1] == 3 && fatals == 0);
    reset(); base.lb_TaskResBase = NULL; TaskLaunch(NULL, &target, &hook);
    CHECK(count == 2 && events[0] == 2 && events[1] == 3 && fatals == 0);
    reset(); base.lb_TaskResBase = NULL; TaskLaunch(NULL, &target, NULL);
    CHECK(count == 1 && events[0] == 3 && fatals == 0);
    reset(); resource.contract ^= 1; TaskLaunch(NULL, &target, &hook);
    CHECK(count == 0 && fatals == 1);
    reset(); resource.prelaunch = NULL; TaskLaunch(NULL, &target, &hook);
    CHECK(count == 0 && fatals == 1);
    reset(); observe_result = 0; TaskLaunch(NULL, &target, &hook);
    CHECK(count == 1 && events[0] == 1 && fatals == 1);
    reset(); ready_result = 0; TaskLaunch(NULL, &target, &hook);
    CHECK(count == 3 && events[0] == 1 && events[1] == 2 && events[2] == 3 && fatals == 1);
    printf("direct-prelaunch: %u checks passed\n", checks);
    return 0;
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    args = parser.parse_args()
    args.artifact_dir.mkdir(parents=True, exist_ok=True)
    source = ROOT / "rom/exec/newaddtask.c"
    interface_path = ROOT / "rom/exec/taskresource_cleanup.h"
    interface = interface_path.read_text()
    contract_match = re.search(
        r"(?m)^#define TASKRESOURCE_CLEANUP_CONTRACT (0x[0-9a-fA-F]+)UL$",
        interface,
    )
    if contract_match is None:
        raise AssertionError("cleanup contract value not found in private interface")
    if "BOOL (*prelaunch)(struct Task *, APTR);" not in interface:
        raise AssertionError("private prelaunch binding is not BOOL-returning")
    contract = contract_match.group(1) + "UL"
    match = re.search(r"(?ms)^static void TaskLaunch\(.*?^}\n", source.read_text())
    if match is None:
        raise AssertionError("actual TaskLaunch body not found")
    body = match.group(0)
    generated = args.artifact_dir / "direct-prelaunch.c"
    generated.write_text(HARNESS.replace("@BODY@", body).replace("@CONTRACT@", contract))
    print("interface", digest(interface_path), interface_path)
    print("source", digest(source), source)
    print("generated", digest(generated), generated)
    for optimization in ("O1", "O2"):
        binary = args.artifact_dir / ("direct-prelaunch-" + optimization)
        command = ["clang", "-std=c11", "-D_DEFAULT_SOURCE", "-" + optimization,
                   "-Wall", "-Wextra", "-Wconversion", "-Wpedantic", "-Werror",
                   "-fno-strict-aliasing", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", str(generated), "-o", str(binary)]
        print("compile", " ".join(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
        print("artifact", binary.stat().st_size, digest(binary), binary, flush=True)
    # A mutant bypassing the observer must fail the ordering assertion.
    mutant = args.artifact_dir / "direct-prelaunch-negative.c"
    changed = body.replace("if (!resource->prelaunch(task, resource))", "if (0)")
    if changed == body:
        raise AssertionError("negative mutation did not match actual callback")
    mutant.write_text(HARNESS.replace("@BODY@", changed).replace("@CONTRACT@", contract))
    binary = args.artifact_dir / "direct-prelaunch-negative"
    subprocess.run(["clang", "-std=c11", "-D_DEFAULT_SOURCE", "-O2", "-Wall", "-Wextra",
                    "-Wconversion", "-Wpedantic", "-Werror",
                    "-fno-strict-aliasing", "-fsanitize=address,undefined", str(mutant),
                    "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], check=False, timeout=20)
    if result.returncode != 1:
        raise AssertionError("missing-observer negative control did not fail as expected")
    print("negative-control: expected assertion exit 1")
    print("Limits: callback, Ready, current-task retention and concurrency are mocked.")


if __name__ == "__main__":
    main()
