#!/bin/sh

set -eu
# macOS shasum is a Perl program and inherits locale variables. The portable C
# locale avoids warnings when the host advertises an unavailable UTF-8 locale.
LC_ALL=C
export LC_ALL

hash_stream() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    else
        echo "no SHA-256 command is available for the build fingerprint" >&2
        exit 1
    fi
}

if [ "${1-}" = "--flags" ]; then
    printf '%s' "${MC_BUILD_FLAGS-}" | hash_stream
    exit 0
fi

for source_path in "$@"; do
    if [ ! -f "$source_path" ]; then
        echo "source fingerprint input is missing: $source_path" >&2
        exit 1
    fi
    printf '%s\000' "$source_path"
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$source_path" | awk '{print $1}'
    else
        sha256sum "$source_path" | awk '{print $1}'
    fi
done | hash_stream
