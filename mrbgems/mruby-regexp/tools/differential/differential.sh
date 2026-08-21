#!/bin/bash
# Generate cases, run them under CRuby and under each mruby given, and compare.
#
#   bash mrbgems/mruby-regexp/tools/differential/differential.sh [gen options --] [name=]mruby...
#
# Everything before `--` goes to gen.rb; what follows (everything, when there
# is no `--`) names the mruby binaries, so master and a branch can be compared
# in one run:
#
#   bash .../differential.sh -n 10000 -s 2 -- master=build-master/host/bin/mruby branch=build/host/bin/mruby
#
# The name before `=` is how the run appears in the report and names its
# output; without one the directory above bin/ is used (host, or the build's
# name). The files go under $OUT (differential.out in the current directory
# unless set): cases.txt, cruby.out and one <name>.out per mruby, then
# compare.rb's report on stdout. RUBY names the CRuby (ruby), MEM_MB caps each
# run's address space in MB (1024), TIMEOUT bounds each run in seconds (600,
# when timeout(1) exists), COMPARE_OPTS goes to compare.rb.
#
# CASES names a cases file to run in place of a generated one, so that a hand
# written list, or the candidates minimize.rb draws from a case, goes through
# the same runs; gen.rb is not called then and the gen options are refused.
# NO_COMPARE=1 stops before compare.rb and leaves the outputs to be read.
#
# A run that dies (the timeout, a crash) is resumed after the case it died on,
# which is recorded as LIMIT killed; the address-space cap is what keeps a
# runaway match on either side from taking the machine, and under it CRuby
# raises RegexpError, which run.rb records, rather than dying.

set -u
here=$(dirname "$0")
OUT=${OUT:-differential.out}
RUBY=${RUBY:-ruby}
MEM_MB=${MEM_MB:-1024}
TIMEOUT=${TIMEOUT:-600}
read -r -a compare_opts <<< "${COMPARE_OPTS:-}"

gen_opts=()
for arg in "$@"; do
  if [ "$arg" = "--" ]; then
    while [ "$1" != "--" ]; do gen_opts+=("$1"); shift; done
    shift
    break
  fi
done
[ $# -ge 1 ] || { echo "usage: $0 [gen options --] [name=]mruby..." >&2; exit 2; }

mkdir -p "$OUT"
if [ -n "${CASES:-}" ]; then
  [ ${#gen_opts[@]} -eq 0 ] || { echo "$0: CASES takes the cases, so gen options are refused" >&2; exit 2; }
  cp "$CASES" "$OUT/cases.txt" || exit 1
else
  "$RUBY" "$here/gen.rb" ${gen_opts[@]+"${gen_opts[@]}"} > "$OUT/cases.txt" || exit 1
fi
total=$(grep -c . "$OUT/cases.txt")

bound=()
if command -v timeout >/dev/null 2>&1; then bound=(timeout "$TIMEOUT"); fi

# run BIN OUTFILE: run.rb under BIN, capped, resumed after a kill
run() {
  local bin=$1 out=$2 done_n now rc
  : > "$out"
  while :; do
    done_n=$(grep -c . "$out")
    [ "$done_n" -ge "$total" ] && break
    (ulimit -v $((MEM_MB * 1024)); ${bound[@]+"${bound[@]}"} "$bin" "$here/run.rb" "$OUT/cases.txt" "$done_n" >> "$out")
    rc=$?
    [ $rc -eq 0 ] && break
    now=$(grep -c . "$out")
    if [ "$now" -lt "$total" ]; then
      printf '%s\tLIMIT killed rc=%s\n' "$(sed -n "$((now + 1))p" "$OUT/cases.txt")" "$rc" >> "$out"
    fi
  done
}

echo "$total cases in $OUT/cases.txt" >&2
run "$RUBY" "$OUT/cruby.out"
outs=()
for arg in "$@"; do
  case $arg in
    *=*) name=${arg%%=*}; bin=${arg#*=} ;;
    *) name=$(basename "$(dirname "$(dirname "$arg")")"); bin=$arg ;;
  esac
  run "$bin" "$OUT/$name.out"
  outs+=("$OUT/$name.out")
done
[ -n "${NO_COMPARE:-}" ] && exit 0
"$RUBY" "$here/compare.rb" ${compare_opts[@]+"${compare_opts[@]}"} "$OUT/cruby.out" "${outs[@]}"
