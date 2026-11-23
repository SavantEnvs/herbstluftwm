#!/usr/bin/env bash
#
# mayhem/test.sh — known-answer functional test of herbstluftwm (built by mayhem/build.sh with
# normal flags in build-test/). Starts a private Xvfb, runs the real window manager on it, drives
# it with herbstclient and compares each reply with the expected output: layout load/dump round
# trips, frame splits, tags, monitors, attributes, settings, meta-commands, completion, rules,
# keybinds and the CSS pretty-printer. (Upstream's own suite, tests/*.py, needs pytest, tox,
# python-xlib, xdotool, Xephyr and more; this script asserts the same behaviour for the code the
# fuzz targets reach.)
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

WM="$SRC/build-test/herbstluftwm"
HC="$SRC/build-test/herbstclient"
for b in "$WM" "$HC"; do
  [ -x "$b" ] || { echo "missing $b — run mayhem/build.sh first" >&2; emit_ctrf hlwm-known-answer 0 1; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/hlwm-test.XXXXXX")
cleanup() { kill "${wm_pid:-}" "${x_pid:-}" 2>/dev/null; wait 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

Xvfb -displayfd 5 -screen 0 800x600x24 -nolisten tcp 5>"$work/display" 2>"$work/xvfb.log" &
x_pid=$!
for _ in $(seq 100); do [ -s "$work/display" ] && break; sleep 0.1; done
export DISPLAY=":$(head -n1 "$work/display")"
"$WM" --autostart /bin/true >"$work/wm.log" 2>&1 &
wm_pid=$!
for _ in $(seq 100); do "$HC" true 2>/dev/null && break; sleep 0.1; done

passed=0; failed=0
# check <name> <expected output> <herbstclient args...>
check() {
  local name="$1" expected="$2"; shift 2
  local got
  got=$("$HC" "$@" 2>&1)
  if [ "$got" == "$expected" ]; then
    passed=$((passed + 1)); echo "PASS $name"
  else
    failed=$((failed + 1)); echo "FAIL $name"; echo "  expected: $(printf '%q' "$expected")"; echo "  got:      $(printf '%q' "$got")"
  fi
}
TAB=$'\t'

check initial-dump '(clients vertical:0)' dump
check load-dump '(split horizontal:0.5:0 (clients max:0) (clients grid:0))' \
  chain , load '(split horizontal:0.5:0 (clients max:0) (clients grid:0))' , dump
check split-dump '(split horizontal:0.5:0 (split vertical:0.3:0 (clients max:0) (clients max:0)) (clients grid:0))' \
  chain , split vertical 0.3 , dump
check load-invalid-rejected $'load: Syntax error at 7: Expected 3 arguments but got 1:\n"(split foo)"\n        ~~~' load '(split foo)'
check tags-count-1 1 attr tags.count
check tags-add 2 chain , add foo , attr tags.count
check compare-true ok and . compare tags.count = 2 . echo ok
check substitute '2' substitute C tags.count echo C
check sprintf '2-default' sprintf X '%s-%s' tags.count tags.focus.name echo X
check complete-merge-tag 'merge_tag' complete 0 merge_t
check setting 7 chain , set frame_gap 7 , get frame_gap
check rule "label=0${TAB}class=Foo${TAB}tag=bar${TAB}" chain , rule class=Foo tag=bar , list_rules
check keybind "Mod4+x${TAB}close" chain , keybind Mod4-x close , list_keybinds
check css-print $'.client > .title {\n    border-width: 2px;\n}' debug_css --print-css '--stylesheet=.client  >.title{border-width:2px}'
check monitors $'0: 400x600+0+0 with tag "default" [FOCUS]\n1: 400x600+400+0 with tag "foo"' \
  chain , set_monitors 400x600+0+0 400x600+400+0 , list_monitors
check user-attr hello chain , new_attr string my_x , set_attr my_x hello , get_attr my_x

emit_ctrf hlwm-known-answer "$passed" "$failed"
