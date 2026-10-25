#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/aros-fetch-normalization.XXXXXX")
trap 'command rm -rf "$task_tmp"' EXIT

fail()
{
    echo "test-fetch-normalization: $*" >&2
    exit 1
}

assert_contains()
{
    local needle="$1" file="$2"
    grep -Fq -- "$needle" "$file" || fail "expected '$needle' in $file"
}

assert_not_contains()
{
    local needle="$1" file="$2"
    if grep -Fq -- "$needle" "$file"; then
        fail "did not expect '$needle' in $file"
    fi
}

expect_fetch_failure()
{
    local label="$1"
    shift
    if AROS_FETCH_OFFLINE=1 bash "$source_root/scripts/fetch.sh" "$@" \
        >"$task_tmp/$label.output" 2>&1; then
        fail "$label unexpectedly succeeded"
    fi
}

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

command mkdir -p "$task_tmp/seed" "$task_tmp/cache"
printf 'canonical fetch fixture\n' > "$task_tmp/seed/payload.txt"
command tar -czf "$task_tmp/cache/zlib.tar.gz" -C "$task_tmp/seed" payload.txt
archive_sha=$(sha256_file "$task_tmp/cache/zlib.tar.gz")
archive_size=$(wc -c < "$task_tmp/cache/zlib.tar.gz" | tr -d '[:space:]')

# The real source declaration must expand through Python genmf and retain its
# concrete fetch target plus canonical-output identity fields.
python3 "$source_root/tools/genmf/genmf.py" \
    "$source_root/config/make.tmpl" \
    "$source_root/workbench/libs/z/mmakefile.src" \
    "$task_tmp/zlib.mk" >"$task_tmp/zlib-genmf.log" 2>&1 || {
        command cat "$task_tmp/zlib-genmf.log" >&2
        fail "Python genmf failed for the zlib source"
    }
assert_contains 'zlib-fetch :' "$task_tmp/zlib.mk"
assert_contains '--normalization "canonical-tar-gzip-v1"' "$task_tmp/zlib.mk"
assert_contains '--normalized-size "2337668"' "$task_tmp/zlib.mk"
assert_contains 'zlib.tar.gz=sha256:883d22e0b9aefc31a383c462adacdbd7941e6862559c5b27e59599db8114e783' "$task_tmp/zlib.mk"

# Evaluate generated recipes without running their commands. The normalized
# source has both flags; a legacy exact-byte recipe expands neither flag.
if command -v gmake >/dev/null 2>&1; then
    make_program=$(command -v gmake)
else
    make_program=$(command -v make) || fail "GNU make is required for macro verification"
fi
command mkdir -p "$task_tmp/top/config"
printf '\n' > "$task_tmp/top/config/make.cfg"
python3 "$source_root/tools/genmf/genmf.py" \
    "$source_root/config/make.tmpl" \
    "$source_root/workbench/libs/jpeg/mmakefile.src" \
    "$task_tmp/jpeg.mk" >"$task_tmp/jpeg-genmf.log" 2>&1 || {
        command cat "$task_tmp/jpeg-genmf.log" >&2
        fail "Python genmf failed for the legacy JPEG source"
    }
assert_contains 'jpeg-fetch :' "$task_tmp/jpeg.mk"

zlib_make_output=$(
    "$make_program" -n -f "$task_tmp/zlib.mk" zlib-fetch \
        "TOP=$task_tmp/top" "SRCDIR=$source_root" \
        "CURDIR=workbench/libs/z" "GENDIR=$task_tmp/generated" \
        "PORTSDIR=$task_tmp/ports" "PORTSSOURCEDIR=$task_tmp/cache" \
        "FETCH=fetch-probe" ECHO=echo IF=if TEST=test TOUCH=touch Q= 2>&1
) || fail "dry-run of generated zlib fetch target failed: $zlib_make_output"
printf '%s\n' "$zlib_make_output" > "$task_tmp/zlib-make.output"
assert_contains '--normalization "canonical-tar-gzip-v1"' "$task_tmp/zlib-make.output"
assert_contains '--normalized-size "2337668"' "$task_tmp/zlib-make.output"
assert_contains 'fetch-probe' "$task_tmp/zlib-make.output"

jpeg_make_output=$(
    "$make_program" -n -f "$task_tmp/jpeg.mk" jpeg-fetch \
        "TOP=$task_tmp/top" "SRCDIR=$source_root" \
        "CURDIR=workbench/libs/jpeg" "GENDIR=$task_tmp/generated" \
        "PORTSDIR=$task_tmp/ports" "PORTSSOURCEDIR=$task_tmp/cache" \
        "FETCH=aros toolchain __metamake-fetch" ECHO=echo IF=if TEST=test TOUCH=touch Q= 2>&1
) || fail "dry-run of generated JPEG fetch target failed: $jpeg_make_output"
printf '%s\n' "$jpeg_make_output" > "$task_tmp/jpeg-make.output"
assert_contains 'aros toolchain __metamake-fetch' "$task_tmp/jpeg-make.output"
assert_not_contains '--normalization' "$task_tmp/jpeg-make.output"
assert_not_contains '--normalized-size' "$task_tmp/jpeg-make.output"

# A pre-seeded, exact canonical cache entry is accepted offline and unpacked.
valid_destination="$task_tmp/valid-destination"
valid_base="$valid_destination"
AROS_FETCH_OFFLINE=1 bash "$source_root/scripts/fetch.sh" \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/cache" -d "$valid_destination" -b "$valid_base" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size" \
    >"$task_tmp/valid-fetch.output" 2>&1 || {
        command cat "$task_tmp/valid-fetch.output" >&2
        fail "verified local canonical cache input was not accepted"
    }
test -f "$valid_destination/payload.txt" || fail "canonical archive was not extracted"
test "$(<"$valid_destination/payload.txt")" = "canonical fetch fixture" || fail "unexpected extracted contents"
test -f "$valid_base/.zlib.tar.gz.unpacked" || fail "unpack completion marker was not written"

if test "${archive_sha:0:1}" = 0; then
    wrong_sha="1${archive_sha:1}"
else
    wrong_sha="0${archive_sha:1}"
fi
expect_fetch_failure wrong-hash \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/cache" -d "$task_tmp/wrong-hash-destination" \
    -cs "zlib.tar.gz=sha256:$wrong_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size"
assert_contains 'SHA-256 mismatch' "$task_tmp/wrong-hash.output"
test "$(sha256_file "$task_tmp/cache/zlib.tar.gz")" = "$archive_sha" || fail "wrong-hash check replaced the cache input"
test ! -e "$task_tmp/cache/zlib.fetch" || fail "wrong-hash check left a fetch lock"
test ! -e "$task_tmp/wrong-hash-destination" || fail "wrong-hash check unpacked or mutated its destination"

wrong_size=$((archive_size + 1))
expect_fetch_failure wrong-size \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/cache" -d "$task_tmp/wrong-size-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$wrong_size"
assert_contains 'normalized-size mismatch' "$task_tmp/wrong-size.output"
test "$(sha256_file "$task_tmp/cache/zlib.tar.gz")" = "$archive_sha" || fail "wrong-size check replaced the cache input"
test ! -e "$task_tmp/cache/zlib.fetch" || fail "wrong-size check left a fetch lock"
test ! -e "$task_tmp/wrong-size-destination" || fail "wrong-size check unpacked or mutated its destination"

missing_location="$task_tmp/missing-cache"
expect_fetch_failure missing-cache \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$missing_location" -d "$task_tmp/missing-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size"
assert_contains 'needs the canonical archive in the local cache' "$task_tmp/missing-cache.output"
test ! -e "$missing_location" || fail "offline canonical cache miss created a location or lock"
test ! -e "$task_tmp/missing-destination" || fail "offline canonical cache miss created an extraction destination"

# Online, a cache miss may be filled from an origin that serves the canonical
# bytes. An origin with other bytes is rejected and the next one is tried.
online_fetch()
{
    local label="$1"
    shift
    AROS_FETCH_OFFLINE=0 bash "$source_root/scripts/fetch.sh" "$@" \
        >"$task_tmp/$label.output" 2>&1
}

noncanonical_origin="$task_tmp/noncanonical-origin"
canonical_origin="$task_tmp/canonical-origin"
mkdir -p "$noncanonical_origin" "$canonical_origin"
printf 'not the canonical archive\n' | gzip -n > "$noncanonical_origin/zlib.tar.gz"
cp "$task_tmp/cache/zlib.tar.gz" "$canonical_origin/zlib.tar.gz"

mirror_location="$task_tmp/mirror-cache"
mirror_destination="$task_tmp/mirror-destination"
if ! online_fetch mirror \
    -ao "$noncanonical_origin $canonical_origin" -a zlib -s tar.gz \
    -l "$mirror_location" -d "$mirror_destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size"; then
    cat "$task_tmp/mirror.output" >&2
    fail "canonical origin was not accepted after a rejected origin"
fi
assert_contains "rejecting 'zlib.tar.gz' from '$noncanonical_origin'" "$task_tmp/mirror.output"
test "$(sha256_file "$mirror_location/zlib.tar.gz")" = "$archive_sha" || fail "mirror fetch cached other bytes"
test -f "$mirror_destination/payload.txt" || fail "mirror fetch did not extract the canonical archive"

rejected_location="$task_tmp/rejected-cache"
if online_fetch rejected \
    -ao "$noncanonical_origin https://127.0.0.1:1/unused" -a zlib -s tar.gz \
    -l "$rejected_location" -d "$task_tmp/rejected-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size"; then
    fail "non-canonical origin was accepted"
fi
assert_contains 'Error while fetching the archive' "$task_tmp/rejected.output"
test ! -e "$rejected_location/zlib.tar.gz" || fail "rejected origin payload stayed in the cache"
test ! -e "$rejected_location/zlib.fetch" || fail "rejected origin left a fetch lock"
test ! -e "$task_tmp/rejected-destination" || fail "rejected origin payload was unpacked"

expect_fetch_failure unknown-policy \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/unknown-policy-cache" -d "$task_tmp/unknown-policy-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" --normalization unknown-v1 \
    --normalized-size "$archive_size"
assert_contains 'unsupported normalization policy' "$task_tmp/unknown-policy.output"
test ! -e "$task_tmp/unknown-policy-cache" || fail "unknown policy mutated the cache path"
test ! -e "$task_tmp/unknown-policy-destination" || fail "unknown policy created an extraction destination"

expect_fetch_failure missing-size \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/missing-size-cache" -d "$task_tmp/missing-size-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" --normalization canonical-tar-gzip-v1
assert_contains 'must be supplied together' "$task_tmp/missing-size.output"
test ! -e "$task_tmp/missing-size-cache" || fail "incomplete representation metadata mutated the cache path"
test ! -e "$task_tmp/missing-size-destination" || fail "incomplete representation metadata created a destination"

expect_fetch_failure missing-policy \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/missing-policy-cache" -d "$task_tmp/missing-policy-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" --normalized-size "$archive_size"
assert_contains 'must be supplied together' "$task_tmp/missing-policy.output"
test ! -e "$task_tmp/missing-policy-cache" || fail "incomplete representation metadata mutated the cache path"
test ! -e "$task_tmp/missing-policy-destination" || fail "incomplete representation metadata created a destination"

expect_fetch_failure zero-size \
    -ao https://127.0.0.1:1/unused -a zlib -s tar.gz \
    -l "$task_tmp/zero-size-cache" -d "$task_tmp/zero-size-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size 0
assert_contains 'positive decimal byte count' "$task_tmp/zero-size.output"
test ! -e "$task_tmp/zero-size-cache" || fail "zero-size input mutated the cache path"
test ! -e "$task_tmp/zero-size-destination" || fail "zero-size input created a destination"

expect_fetch_failure multiple-candidates \
    -ao https://127.0.0.1:1/unused -a zlib -s 'tar.gz tgz' \
    -l "$task_tmp/multiple-cache" -d "$task_tmp/multiple-destination" \
    -cs "zlib.tar.gz=sha256:$archive_sha zlib.tgz=sha256:$archive_sha" \
    --normalization canonical-tar-gzip-v1 --normalized-size "$archive_size"
assert_contains 'requires exactly one archive suffix candidate' "$task_tmp/multiple-candidates.output"
test ! -e "$task_tmp/multiple-cache" || fail "multiple candidates mutated the cache path"
test ! -e "$task_tmp/multiple-destination" || fail "multiple candidates created a destination"

echo "test-fetch-normalization: passed"
