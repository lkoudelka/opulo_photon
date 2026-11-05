#!/bin/sh
# Behavioural equivalence check of photon/src/PhotonFeeder.cpp:
#   tools/equivalence/run.sh [BASE_REV]     (default BASE_REV = dev)
# Builds the harness with PhotonFeeder from BASE_REV and from the working
# tree, runs both and compares the traces.  Exit 0 = identical.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASE=${1:-dev}
WORK=${WORK:-$(mktemp -d)}
mkdir -p "$WORK/base"
for f in PhotonFeeder.cpp PhotonFeeder.h define.h versions.h; do
    git -C "$ROOT" show "$BASE:photon/src/$f" > "$WORK/base/$f"
done
CXX=${CXX:-c++}
FLAGS="-std=gnu++17 -O1 -DVERSION_STRING=\"v1.0.7\" -I$HERE/mock -w"
$CXX $FLAGS -I"$WORK/base" -o "$WORK/sim_base" "$HERE/sim.cpp" "$WORK/base/PhotonFeeder.cpp"
$CXX $FLAGS -I"$ROOT/photon/src" -o "$WORK/sim_new" "$HERE/sim.cpp" "$ROOT/photon/src/PhotonFeeder.cpp"
"$WORK/sim_base" "$WORK/trace_base.txt"
"$WORK/sim_new" "$WORK/trace_new.txt"
echo "trace lines: $(wc -l < "$WORK/trace_base.txt") base, $(wc -l < "$WORK/trace_new.txt") new  (work: $WORK)"
if cmp -s "$WORK/trace_base.txt" "$WORK/trace_new.txt"; then
    echo "EQUIVALENT: identical traces ($BASE vs working tree)"
else
    echo "DIFFERENT:"; diff "$WORK/trace_base.txt" "$WORK/trace_new.txt" | head -40; exit 1
fi
