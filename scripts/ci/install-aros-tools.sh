#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo 'usage: install-aros-tools.sh DESTINATION' >&2
    exit 64
fi

destination=$1
version=v0.6.7
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64)
        target=x86_64-unknown-linux-gnu
        expected_sha256=80fd10728a942bf81a733758b318876aa3c8e842e0fd84ed9371d061df11478a
        expected_size=25175825
        ;;
    Linux/aarch64)
        target=aarch64-unknown-linux-gnu
        expected_sha256=f790db781a71cbeb121705fafa33cbc884a5d453be19aec975bf70942585b93c
        expected_size=24162103
        ;;
    Darwin/arm64)
        target=aarch64-apple-darwin
        expected_sha256=5e2c1719f18676e3748ecf0e0d2a952d779c851890e7f382e82b705ec4a9e15f
        expected_size=22549014
        ;;
    *)
        echo "Unsupported aros-tools CI host: $(uname -s)/$(uname -m)" >&2
        exit 65
        ;;
esac

if [[ -e "$destination" ]]; then
    echo "Refusing to overwrite existing runtime destination: $destination" >&2
    exit 66
fi
mkdir -p "$destination"
archive_name="aros-tools-${version}-${target}.tar.gz"
archive="$destination/$archive_name"
curl --fail --location --silent --show-error --retry 3 --retry-all-errors \
    --max-time 120 --proto '=https' --tlsv1.2 \
    "https://github.com/metaneutrons/aros-tools/releases/download/${version}/${archive_name}" \
    --output "$archive"

actual_size=$(wc -c < "$archive" | tr -d '[:space:]')
if [[ "$actual_size" != "$expected_size" ]]; then
    echo "Unexpected aros-tools archive size: $actual_size" >&2
    exit 67
fi
printf '%s  %s\n' "$expected_sha256" "$archive" | shasum -a 256 --check --status

tar -xzf "$archive" -C "$destination"
bin_dir="$destination/aros-tools-${version}-${target}/bin"
for program in aros aros-fetch aros-genmodule aros-transpiler aros-verify; do
    if [[ ! -f "$bin_dir/$program" || ! -x "$bin_dir/$program" ]]; then
        echo "Missing executable in verified aros-tools archive: $program" >&2
        exit 68
    fi
done
if [[ -n "${GITHUB_PATH:-}" ]]; then
    printf '%s\n' "$bin_dir" >> "$GITHUB_PATH"
fi
if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'AROS_TOOLS_BIN=%s\n' "$bin_dir" >> "$GITHUB_ENV"
fi
printf 'Installed aros-tools %s for %s (%s bytes, SHA-256 %s)\n' \
    "$version" "$target" "$expected_size" "$expected_sha256"
