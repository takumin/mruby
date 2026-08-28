#!/bin/bash
# Walk every codepoint under CRuby and under each mruby given, and report the
# classes whose membership parts.
#
#   bash mrbgems/mruby-regexp/tools/differential/classes.sh [classes options --] [name=]mruby...
#
# Everything before `--` goes to classes.rb (`-m` narrows the codepoints, `-p`
# the property names, `-s` the ranges printed per class); what follows names
# the mruby binaries, as differential.sh takes them. The files go under $OUT
# (classes.out in the current directory unless set): cruby.out and one
# <name>.out per mruby, then the report on stdout. RUBY names the CRuby
# (ruby), MEM_MB caps each run's address space in MB (1024).
#
# A walk is one process per run over the whole of Unicode, a few minutes each
# with the properties in; -m and -p narrow it while a difference is being
# chased, and -p '' leaves the properties out. MEM_MB caps the walks and not
# the report, which holds every run's membership at once and reaches some
# gigabytes at the full walk.

set -u
here=$(dirname "$0")
OUT=${OUT:-classes.out}
RUBY=${RUBY:-ruby}
MEM_MB=${MEM_MB:-1024}

walk_opts=()
for arg in "$@"; do
  if [ "$arg" = "--" ]; then
    while [ "$1" != "--" ]; do walk_opts+=("$1"); shift; done
    shift
    break
  fi
done
[ $# -ge 1 ] || { echo "usage: $0 [classes options --] [name=]mruby..." >&2; exit 2; }

mkdir -p "$OUT"

# walk BIN OUTFILE: classes.rb under BIN, capped
walk() {
  (ulimit -v $((MEM_MB * 1024)); "$1" "$here/classes.rb" ${walk_opts[@]+"${walk_opts[@]}"} > "$2") || exit 1
}

walk "$RUBY" "$OUT/cruby.out"
outs=()
for arg in "$@"; do
  case $arg in
    *=*) name=${arg%%=*}; bin=${arg#*=} ;;
    *) name=$(basename "$(dirname "$(dirname "$arg")")"); bin=$arg ;;
  esac
  walk "$bin" "$OUT/$name.out"
  outs+=("$OUT/$name.out")
done
"$RUBY" "$here/classes.rb" ${walk_opts[@]+"${walk_opts[@]}"} --compare "$OUT/cruby.out" "${outs[@]}"
