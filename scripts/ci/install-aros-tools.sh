#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo 'usage: install-aros-tools.sh DESTINATION' >&2
    exit 64
fi

destination=$1
version=v0.6.10
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64)
        target=x86_64-unknown-linux-gnu
        expected_sha256=dcb1ebc46e61c6f2c98fb4cf3c9732ed6bdd0fa16182ecd60d48b96712404346
        expected_size=25233820
        ;;
    Linux/aarch64)
        target=aarch64-unknown-linux-gnu
        expected_sha256=8f73ff7dff4c76e05bb3aad7f8c135fe9855a1524f3ca66c19cf2228ef2ad6f4
        expected_size=24192407
        ;;
    Darwin/arm64)
        target=aarch64-apple-darwin
        expected_sha256=ff806ce409a8c10b1bc473745d78314fa4921e6b58276df0259ebce6ce5e47db
        expected_size=22633529
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
