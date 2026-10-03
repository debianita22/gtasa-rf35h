#!/bin/sh
# verify-upstream.sh -- upstream/ is gtasa_nx 3460c75, byte for byte
#
#   tools/verify-upstream.sh [GTASA_NX_CHECKOUT]
#
# upstream/FILES.git-blobs lists the git blob id of every vendored file as
# it is in gtasa_nx at 3460c75. Each file's id is recomputed (git
# hash-object), and with a checkout of gtasa_nx the list itself is checked
# against `git ls-tree` of that commit.
set -eu

cd "$(dirname "$0")/../upstream"
fail=0
n=0
while read -r blob path; do
  n=$((n + 1))
  if [ ! -f "$path" ]; then
    echo "missing: $path"
    fail=1
  elif [ "$(git hash-object "$path")" != "$blob" ]; then
    echo "modified: $path"
    fail=1
  fi
done < FILES.git-blobs

extra=$(find . -type f ! -name FILES.git-blobs | sed 's|^\./||' | sort |
  while read -r f; do grep -qF "  $f" FILES.git-blobs || echo "$f"; done)
[ -z "$extra" ] || { echo "not in the list: $extra"; fail=1; }

if [ -n "${1:-}" ]; then
  git -C "$1" ls-tree -r 3460c75 | awk '{ print $3 "  " $4 }' > "${TMPDIR:-/tmp}/gtasa_nx.blobs"
  while read -r blob path; do
    grep -qx "$blob  $path" "${TMPDIR:-/tmp}/gtasa_nx.blobs" || { echo "not in gtasa_nx 3460c75: $path"; fail=1; }
  done < FILES.git-blobs
fi

[ $fail = 0 ] && echo "upstream: $n files identical to gtasa_nx 3460c75" || { echo "upstream: MODIFIED"; exit 1; }
