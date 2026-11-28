#!/bin/zsh
# Bounded host tests compile and exercise the actual board OTA client source.
set -eu
setopt NO_CLOBBER

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
test_source="$test_root/tools/test-board-ota-client.c"
client_source="$test_root/boardtool/ota_client.c"
evidence_root=${EVIDENCE_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$evidence_root"
evidence_dir=$(mktemp -d "$evidence_root/board-ota-client.XXXXXXXX")
cc=${CC:-cc}

run_case() {
    local name="$1"
    local optimization="$2"
    local sanitized="$3"
    local binary="$evidence_dir/$name"
    local build_log="$evidence_dir/$name.build.log"
    local run_log="$evidence_dir/$name.run.log"
    local case_exit
    local summary

    if [[ "$sanitized" == yes ]]; then
        if "$cc" -std=c11 -Wall -Wextra -Werror -O"$optimization" \
            -fsanitize=address,undefined -fno-omit-frame-pointer \
            "$test_source" "$client_source" -o "$binary" \
            > "$build_log" 2>&1; then
            :
        else
            case_exit=$?
            print -r -- "$name: compile_exit=$case_exit"
            cat "$build_log"
            return "$case_exit"
        fi
        if ASAN_OPTIONS=halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$binary" > "$run_log" 2>&1; then
            :
        else
            case_exit=$?
            print -r -- "$name: run_exit=$case_exit"
            cat "$run_log"
            return "$case_exit"
        fi
    else
        if "$cc" -std=c11 -Wall -Wextra -Werror -O"$optimization" \
            "$test_source" "$client_source" -o "$binary" \
            > "$build_log" 2>&1; then
            :
        else
            case_exit=$?
            print -r -- "$name: compile_exit=$case_exit"
            cat "$build_log"
            return "$case_exit"
        fi
        if "$binary" > "$run_log" 2>&1; then
            :
        else
            case_exit=$?
            print -r -- "$name: run_exit=$case_exit"
            cat "$run_log"
            return "$case_exit"
        fi
    fi

    summary=$(tail -n 1 "$run_log")
    print -r -- "$name: compile_exit=0 run_exit=0 | $summary"
}

print -r -- "Evidence directory: $evidence_dir"
print -r -- "Compiler: $cc"
print -r -- "Workspace: $repo_root"
run_case O0 0 no
run_case O2 2 no
run_case O0-asan-ubsan 0 yes
run_case O2-asan-ubsan 2 yes
