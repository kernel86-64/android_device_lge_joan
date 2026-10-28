#!/usr/bin/env bash
# Apply the joan patches to the other projects of a LineageOS 23.2 tree.
# Run from the top of the tree after repo sync. Patches already applied
# are skipped, so it is safe to run again after every sync.
set -euo pipefail

TOP=${1:-$(pwd)}
PATCHES=$(cd "$(dirname "$0")" && pwd)/patches
status=0

while IFS= read -r patch; do
    rel=${patch#"$PATCHES"/}
    project=$(dirname "$rel")
    if [[ ! -d $TOP/$project ]]; then
        echo "MISSING  $project (is this the top of a lineage-23.2 tree?)"
        status=1
    elif git -C "$TOP/$project" apply --reverse --check "$patch" 2>/dev/null; then
        echo "applied  $rel"
    elif git -C "$TOP/$project" apply --check "$patch" 2>/dev/null; then
        git -C "$TOP/$project" apply "$patch"
        echo "APPLY    $rel"
    else
        echo "FAILED   $rel (does not apply cleanly)"
        status=1
    fi
done < <(find "$PATCHES" -name '*.patch' | sort)

exit $status
