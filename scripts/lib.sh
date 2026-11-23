# shellcheck shell=bash
# Shared helpers for the mothlight scripts.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${EOSM_WORK:-$ROOT/work}"      # engine checkouts live here (git-ignored)
OUT="${EOSM_OUT:-$ROOT/out}"          # build results
PLATFORM="EOSM.202"                   # Canon EOS M, firmware 2.0.2 -- the only target

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# engine_field <name> <column 2..4>  -> repo / branch / commit from engines.lock
engine_field() {
    awk -v n="$1" -v c="$2" '$1 == n { print $c }' "$ROOT/engines.lock"
}

engine_dir() { echo "$WORK/$1"; }

jobs() { nproc 2>/dev/null || echo 4; }
