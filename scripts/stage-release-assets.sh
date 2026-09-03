#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
	echo "usage: $0 DIST_DIR ARTIFACTS_ROOT OWNER/REPOSITORY RELEASE_TAG COMPONENT" >&2
	exit 2
fi

source_dir=$1
artifact_root=$2
repository=$3
release_tag=$4
component=$5

case "$repository" in
	*/*) ;;
	*) echo "repository must be OWNER/NAME" >&2; exit 2 ;;
esac
case "$release_tag" in
	*[!A-Za-z0-9._-]*|'') echo "invalid release tag" >&2; exit 2 ;;
esac

destination="$artifact_root/$repository/$release_tag"
mkdir -p "$destination"
found=0
for source in "$source_dir/$component-"*; do
	[ -f "$source" ] || continue
	case "$(basename "$source" | tr '[:upper:]' '[:lower:]')" in
		*kernel*|*initramfs*|*initrd*|*vmlinuz*)
			echo "refusing to stage kernel/initramfs asset: $source" >&2
			exit 1
			;;
	esac
	install -m0644 "$source" "$destination/$(basename "$source")"
	found=1
done
[ "$found" -eq 1 ] || { echo "no $component assets in $source_dir" >&2; exit 1; }
echo "$destination"
