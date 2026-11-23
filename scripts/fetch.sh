#!/usr/bin/env bash
# Check out the pinned engine(s) into work/ and apply our patches.
#   scripts/fetch.sh             -> cropmood (the default engine)
#   scripts/fetch.sh upstream    -> magiclantern_simplified dev
#   scripts/fetch.sh all
set -euo pipefail
source "$(dirname "$0")/lib.sh"

fetch_one() {
    local name="$1"
    local repo branch commit dir
    repo="$(engine_field "$name" 2)"; branch="$(engine_field "$name" 3)"; commit="$(engine_field "$name" 4)"
    [ -n "$repo" ] || die "unknown engine '$name' (see engines.lock)"
    dir="$(engine_dir "$name")"

    if [ ! -d "$dir/.git" ]; then
        say "cloning $name ($branch)"
        mkdir -p "$WORK"
        git clone --branch "$branch" --single-branch --depth 50 "$repo" "$dir"
    fi
    if ! git -C "$dir" cat-file -e "$commit^{commit}" 2> /dev/null; then
        say "fetching history for $commit"
        git -C "$dir" fetch --depth 500 origin "$branch"
    fi

    say "$name: resetting to $commit"
    git -C "$dir" reset -q --hard "$commit"
    git -C "$dir" clean -qfdx

    for p in "$ROOT/patches/$name"/*.patch; do
        [ -e "$p" ] || continue
        say "$name: applying $(basename "$p")"
        git -C "$dir" apply --whitespace=nowarn "$p"
    done
}

case "${1:-cropmood}" in
    all) fetch_one cropmood; fetch_one upstream ;;
    *)   fetch_one "${1:-cropmood}" ;;
esac
