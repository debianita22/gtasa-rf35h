#!/bin/sh
# mutate_runner.sh -- prove test_runner.sh catches a broken rf35h-gtasa
#
#   tests/mutate_runner.sh BUILD_DIR
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_runner.sh BUILD_DIR}
M="$PWD/$B/rmutants"
SRC=packaging/lakka/gtasa/scripts/rf35h-gtasa
mkdir -p "$M"
fail=0

mutant() {
  name=$1
  shift
  mkdir -p "$M/$name/packaging/lakka/gtasa/scripts" "$M/$name/tests"
  sed "$@" "$SRC" > "$M/$name/$SRC"
  chmod +x "$M/$name/$SRC"
  if cmp -s "$SRC" "$M/$name/$SRC"; then
    echo "mutant $name: the mutation no longer applies"
    fail=1
    return
  fi
  cp tests/test_runner.sh "$M/$name/tests/"
  if (cd "$M/$name" && sh tests/test_runner.sh build) > "$M/$name.log" 2>&1; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    echo "mutant $name: detected ($(grep -m1 FAIL "$M/$name.log" | sed 's/^ *//'))"
  fi
}

mutant dry-applies -e 's/  \[ -n "${2:-}" \] || set_opt "$cfgkey" "$cfgval"/  set_opt "$cfgkey" "$cfgval"/'
mutant no-trap -e "s/  trap ':' TERM INT/  :/"
mutant ignore-trap -e "s/  trap ':' TERM INT/  trap '' TERM INT/"
mutant minutes -e 's|\$((total % 3600 / 60))|$((total / 60))|'
mutant count-up -e 's/  is_num "$count" || count=1/  count=$((${count:-0} + 1))/'
mutant no-check -e 's/  out=$("$GTASA_BIN" -d "$GTASA_DIR" --check 2>&1)/  out=OK/'
mutant slim-all -e 's/    echo "$used" | grep -qx "$fmt" \&\& continue/    :/'
mutant keep-error -e 's|  rm -f "$GTASA_DIR/last-error.txt"\n  exec|XX|' -e '/^cmd_start/,/^}/ s|  rm -f "$GTASA_DIR/last-error.txt"|  :|'

mutant wall-clock -e 's/  t0=$(uptime_s)/  t0=$(date +%s)/' -e 's/  t1=$(uptime_s)/  t1=$(date +%s)/'
mutant no-tilde -e 's/  case "$v" in "~"\*) .*/  :/'
mutant playlist-not-logs -e "s|    printf '%s/logs' \"\${d:-/storage/playlists}\"|    printf '%s' \"\${d:-/storage/playlists}\"|"
mutant runtime-dir-ignored -e 's/  d=$(ra_cfg runtime_log_directory)/  d=""/'
mutant no-lock-fd -e 's/  GTASA_LOCK_FD=9 "$GTASA_BIN"/  "$GTASA_BIN"/'

[ $fail = 0 ] && echo "runner mutants: all detected" || { echo "runner mutants: FAILED"; exit 1; }
