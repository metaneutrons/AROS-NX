#!/bin/sh
# Portable and fake-native tests only: never opens USB, serial or a target.
set -eu
if [ "$#" -ne 1 ]; then echo "usage: sh run-tests.sh NEW_OUTPUT" >&2; exit 2; fi
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=$1
test ! -e "$output"
test ! -L "$output"
mkdir "$output"
export PYTHONDONTWRITEBYTECODE=1 ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
python3 "$src/client.py" --self-test
for optimization in 0 2; do
    cc=${CC:-clang}
    flags="-std=c11 -Wall -Wextra -Werror -O$optimization -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    "$cc" $flags -I"$src" "$src/protocol.c" "$src/test_protocol.c" -o "$output/protocol-O$optimization"
    "$cc" $flags -DRD_SERVICE_TEST -I"$src" "$src/protocol.c" "$src/service.c" "$src/path.c" "$src/test_service.c" -o "$output/service-O$optimization"
    # files fixture includes the production source to exercise private walkers.
    "$cc" $flags -DRD_FILES_TEST -I"$src" "$src/path.c" "$src/test_files.c" -o "$output/files-O$optimization"
    "$cc" $flags -DRD_SCREEN_HOST_TEST -I"$src" -I"$src/tests" "$src/screen_aros.c" "$src/tests/screen_test_fake.c" "$src/tests/screen_test.c" -o "$output/screen-O$optimization"
    "$cc" $flags -DRD_SERVICE_TEST -Dmain=rd_fixture_main -I"$src" -c "$src/test_service.c" -o "$output/pipe-fakes-O$optimization.o"
    "$cc" $flags -DRD_SERVICE_TEST -I"$src" "$src/protocol.c" "$src/service.c" "$src/path.c" "$src/test_pipe_server.c" "$output/pipe-fakes-O$optimization.o" -o "$output/pipe-server-O$optimization"
    "$output/protocol-O$optimization"
    "$output/service-O$optimization"
    "$output/files-O$optimization"
    "$output/screen-O$optimization"
    python3 "$src/test_pipe_client.py" "$output/pipe-server-O$optimization"
done
echo 'RemoteDiag offline tests pass; native capture/input/DOS/USB behavior remains unqualified'
