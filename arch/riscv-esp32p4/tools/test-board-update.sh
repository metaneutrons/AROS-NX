#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-board-update-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/test-mutant" "$test_tmp/update-mutant.c" "$test_tmp/mutant-output" "$test_tmp/test-budget-mutant" "$test_tmp/update-budget-mutant.c" "$test_tmp/budget-mutant-output" "$test_tmp/update.o"; rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer -I"$test_root/boardtool" \
		"$test_root/boardtool/update.c" \
		"$test_root/tools/test-board-update.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done

if ! grep -Fq 'if (!result->begin_attempted)' \
	"$test_root/boardtool/update.c"; then
	echo "negative-control source anchor is missing" >&2
	exit 1
fi
sed 's/if (!result->begin_attempted)/if (1)/' \
	"$test_root/boardtool/update.c" > "$test_tmp/update-mutant.c"
if ! grep -Fq 'if (1)' "$test_tmp/update-mutant.c"; then
	echo "negative-control mutant was not generated" >&2
	exit 1
fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	-O0 -fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$test_root/boardtool" \
	"$test_tmp/update-mutant.c" \
	"$test_root/tools/test-board-update.c" \
	-o "$test_tmp/test-mutant"
if "$test_tmp/test-mutant" > "$test_tmp/mutant-output" 2>&1; then
	cat "$test_tmp/mutant-output" >&2
	echo "negative-control mutant unexpectedly passed" >&2
	exit 1
else
	mutant_status=$?
fi
if [ "$mutant_status" -ne 1 ]; then
	cat "$test_tmp/mutant-output" >&2
	echo "negative-control mutant exited $mutant_status, expected 1" >&2
	exit 1
fi
if ! grep -Fq 'result->finish_mode == expected_mode' \
	"$test_tmp/mutant-output"; then
	cat "$test_tmp/mutant-output" >&2
	echo "negative-control mutant failed for an unexpected reason" >&2
	exit 1
fi
echo "board update negative control: NO_BEGIN terminal-mode mutant rejected with exit 1"

if ! grep -Fq '(finishing ? 0u : plan->finish_reserve_us)' \
	"$test_root/boardtool/update.c"; then
	echo "aggregate-budget negative-control source anchor is missing" >&2
	exit 1
fi
sed 's/(finishing ? 0u : plan->finish_reserve_us)/(finishing ? 0u : 0u)/' \
	"$test_root/boardtool/update.c" > "$test_tmp/update-budget-mutant.c"
if ! grep -Fq 'plan->total_budget_us -' "$test_tmp/update-budget-mutant.c" || \
	grep -Fq '(finishing ? 0u : plan->finish_reserve_us)' \
		"$test_tmp/update-budget-mutant.c"; then
	echo "aggregate-budget negative-control mutant was not generated" >&2
	exit 1
fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	-O0 -fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$test_root/boardtool" \
	"$test_tmp/update-budget-mutant.c" \
	"$test_root/tools/test-board-update.c" \
	-o "$test_tmp/test-budget-mutant"
if "$test_tmp/test-budget-mutant" > "$test_tmp/budget-mutant-output" 2>&1; then
	cat "$test_tmp/budget-mutant-output" >&2
	echo "aggregate-budget negative-control mutant unexpectedly passed" >&2
	exit 1
else
	budget_mutant_status=$?
fi
if [ "$budget_mutant_status" -ne 1 ]; then
	cat "$test_tmp/budget-mutant-output" >&2
	echo "aggregate-budget negative-control mutant exited $budget_mutant_status, expected 1" >&2
	exit 1
fi
if ! grep -Fq '== P4_BOARD_UPDATE_BUDGET_EXPIRED' \
	"$test_tmp/budget-mutant-output"; then
	cat "$test_tmp/budget-mutant-output" >&2
	echo "aggregate-budget negative control failed for an unexpected reason" >&2
	exit 1
fi
echo "board update negative control: ignored finish reserve rejected with exit 1"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -fno-stack-protector -O2 \
	-c "$test_root/boardtool/update.c" -o "$test_tmp/update.o"
undefined_imports=$("${NM:-nm}" -u "$test_tmp/update.o" | \
	awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }')
if [ -n "$undefined_imports" ]; then
	echo "unexpected undefined imports in update.o: $undefined_imports" >&2
	exit 1
fi
