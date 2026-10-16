#!/usr/bin/env bash
# Line coverage for this repository, recomputed from a clean instrumented run.
#
# WHY PER-REPO. A coverage number means nothing without the scope it was taken
# in, and this chain proves it: measured against the umbrella's query-path
# harness, parser_ddl.cpp reads 49.7% and selectivity.cpp reads 0.00%; measured
# where their tests actually live, they are 81.8% and 67.1%. The umbrella also
# builds this repo with its own tests switched off. So each repo measures
# itself, against its own baseline, and says so in the output.
#
# WHAT IT DOES NOT CLAIM. Coverage says what was REACHED, never what was
# CHECKED. The worked example in this project: changing a join's merge
# predicate from `=` to `>` left every test green while running through fully
# covered lines. The falsifiability gate decides whether reaching a line means
# anything; this only finds lines nothing reaches at all.
#
# THE RATCHET. One total, compared against scripts/coverage-baseline.txt, which
# is a RECORD of what was measured rather than a target anyone picked. Move it
# in the commit that changes coverage, not afterwards to turn a job green.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-coverage}"
BASELINE="$ROOT/scripts/coverage-baseline.txt"

command -v gcov >/dev/null 2>&1 || { echo "coverage: gcov not found"; exit 77; }

# This repo consumes the parser through find_package(DB25Parser), so a
# standalone build needs an INSTALLED parser and CMAKE_PREFIX_PATH pointing at
# it - the same thing every job in .github/workflows/ci.yml does before
# building. Without it cmake fails with a message about DB25Parser not being
# installed, which is easy to misread as a coverage problem.
PREFIX_ARG=()
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
    PREFIX_ARG=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

echo "== configuring instrumented build =="
if ! cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug \
      "${PREFIX_ARG[@]}" \
      -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" >/dev/null 2>&1; then
    echo "coverage: configure failed."
    echo "  This repo needs an installed db25-sql-parser. Build and install it,"
    echo "  then re-run with CMAKE_PREFIX_PATH=<install prefix>."
    exit 1
fi
# Build noise discarded, stderr included: a run that printed every warning would
# bury the one table it exists to produce.
cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>&1 || {
    echo "coverage: the instrumented build failed"; exit 1; }

# A stale .gcda silently merges two runs into one set of counts.
find "$BUILD" -name '*.gcda' -delete
echo "== running the suite =="
ctest --test-dir "$BUILD" --output-on-failure >/dev/null 2>&1 || {
    echo "coverage: the suite itself failed; coverage of a red suite means nothing"
    exit 1; }

echo
echo "== line coverage: the semantic analyzer =="
echo

report=$(for f in $(find "$BUILD" -name '*.gcda'); do
    d=$(dirname "$f"); g=$(basename "$f")
    (cd "$d" && gcov -n "$g" 2>/dev/null) | awk '
        /^File / { f=$2; gsub(/'"'"'/,"",f); next }
        /^Lines executed/ && f ~ /\.cpp$/ && f !~ /\/usr\// { print f"\t"$0; f="" }'
done | sort -u | grep -E 'Semantic-Analyzer/src/')

echo "$report" | sed "s|$ROOT/||" | awk -F'\t' '
    { split($2,a,":"); split(a[2],b,"% of "); printf "  %-52s %6.2f%%  of %5d\n", $1, b[1], b[2] }' | sort -k2 -n

total=$(echo "$report" | awk -F'\t' '
    { split($2,a,":"); split(a[2],b,"% of "); cov += b[1]*b[2]/100; tot += b[2] }
    END { if (tot) printf "%.1f", 100*cov/tot; else print "0" }')
lines=$(echo "$report" | awk -F'\t' '{ split($2,a,":"); split(a[2],b,"% of "); tot += b[2] } END { print tot+0 }')

echo
echo "  TOTAL: ${total}% of ${lines} lines"
[ -r "$BASELINE" ] || { echo "coverage: no baseline at $BASELINE"; exit 1; }
base=$(grep -oE '^[0-9]+(\.[0-9]+)?' "$BASELINE" | head -1)
echo "  BASELINE: ${base}%"

if awk -v t="$total" -v b="$base" 'BEGIN { exit !(t < b) }'; then
    echo
    echo "COVERAGE REGRESSION: ${total}% is below the recorded baseline of ${base}%."
    echo "  Either the change removed tests, or it added code nothing reaches."
    echo "  If the drop is intended, move the number in $BASELINE in THIS commit."
    exit 1
fi
echo "  OK: at or above baseline."
