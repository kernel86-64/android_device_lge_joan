#!/usr/bin/env bash
# Apply the joan patches to the other projects of a LineageOS 24.0 tree.
# Run from the top of the tree after repo sync. Patches already applied
# are skipped, so it is safe to run again after every sync.
#
# A patch directory with a BASE file is a commit series (the kernel): it is
# applied with git am on top of that commit, and TREE holds the tree the
# series must produce.
set -euo pipefail

TOP=${1:-$(pwd)}
PATCHES=$(cd "$(dirname "$0")" && pwd)/patches
status=0

apply_series() {
    local project=$1 dir=$PATCHES/$1 base tree
    base=$(<"$dir/BASE")
    tree=$(<"$dir/TREE")

    if [[ $(git -C "$TOP/$project" rev-parse 'HEAD^{tree}') == "$tree" ]]; then
        echo "applied  $project (series)"
    elif [[ $(git -C "$TOP/$project" rev-parse HEAD) == "$base" ]]; then
        git -C "$TOP/$project" -c user.name='kernel86-64' \
            -c user.email='87580651+kernel86-64@users.noreply.github.com' \
            am -q --keep-cr "$dir"/*.patch
        if [[ $(git -C "$TOP/$project" rev-parse 'HEAD^{tree}') != "$tree" ]]; then
            echo "FAILED   $project series applied but the result differs"
            status=1
            return
        fi
        echo "APPLY    $project ($(find "$dir" -name '*.patch' | wc -l) patches)"
    else
        echo "FAILED   $project is not at $base; run repo sync first"
        status=1
    fi
}

while IFS= read -r dir; do
    project=${dir#"$PATCHES"/}
    if [[ ! -d $TOP/$project ]]; then
        echo "MISSING  $project (is this the top of a lineage-24.0 tree?)"
        status=1
    elif [[ -f $dir/BASE ]]; then
        apply_series "$project"
    else
        for patch in "$dir"/*.patch; do
            rel=${patch#"$PATCHES"/}
            if git -C "$TOP/$project" apply --reverse --check "$patch" 2>/dev/null; then
                echo "applied  $rel"
            elif git -C "$TOP/$project" apply --check "$patch" 2>/dev/null; then
                git -C "$TOP/$project" apply "$patch"
                echo "APPLY    $rel"
            else
                echo "FAILED   $rel (does not apply cleanly)"
                status=1
            fi
        done
    fi
done < <(find "$PATCHES" -name '*.patch' -printf '%h\n' | sort -u)

exit $status
