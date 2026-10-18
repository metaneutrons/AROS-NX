#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo 'usage: install-aros-tools.sh DESTINATION' >&2
    exit 64
fi

destination=$1
version=v0.6.0
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64)
        target=x86_64-unknown-linux-gnu
        expected_sha256=19b28af05609eb2d633270c8a96de9cb9cdd862955216d20ecef13d2810b27a2
        expected_size=25161236
        ;;
    Linux/aarch64)
        target=aarch64-unknown-linux-gnu
        expected_sha256=f109c07311530e253c33b9e7accbb2fee2d85d4740d644eea2954ea4ec4fb07b
        expected_size=24114766
        ;;
    Darwin/arm64)
        target=aarch64-apple-darwin
        expected_sha256=fc19d86a609185fba3d7356e21bb50381252fb2ce8cd5c7fcd5fcda16d335be0
        expected_size=22540338
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
