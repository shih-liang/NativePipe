#!/bin/sh
# Hash every checked-in input that can change the NativePipe compositor build.
# The path and file hash are both included so renames cannot reuse an old stamp.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

hash_stream()
{
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum
	else
		shasum -a 256
	fi
}

hash_file()
{
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

(
	cd "$ROOT"
	find . ../encoder ../../common/file_rpc -type f \( -name '*.c' -o -name '*.h' -o -name '*.xml' \
		-o -name Makefile -o -name source-hash.sh \) \
		! -path './.build/*' ! -path './dist/*' | LC_ALL=C sort |
	while IFS= read -r file; do
		printf '%s  %s\n' "$(hash_file "$file")" "$file"
	done
) | hash_stream | awk '{print $1}'
