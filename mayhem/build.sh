#!/usr/bin/env bash
# mayhem/build.sh — build herbstluftwm's fuzz harnesses and the functional-test build.
#
#   build-fuzz/  upstream's CMake build with $SANITIZER_FLAGS $DEBUG_FLAGS; its object files
#                (all of src/ except main.cpp) are linked into each harness. The harnesses
#                compile src/main.cpp themselves (main renamed) to reuse upstream's command table.
#   build-test/  upstream's CMake build with normal flags: herbstluftwm + herbstclient for
#                mayhem/test.sh.
# Both directories match upstream's `build*/` .gitignore entry.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# 1) Sanitized upstream build (objects reused by the harnesses).
cmake -S . -B build-fuzz -DWITH_DOCUMENTATION=OFF -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS" \
      -DCMAKE_CXX_FLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$SANITIZER_FLAGS"
cmake --build build-fuzz -j"$MAYHEM_JOBS" --target herbstluftwm

OBJDIR=build-fuzz/src/CMakeFiles/herbstluftwm.dir
mapfile -t OBJS < <(find "$OBJDIR" -name '*.cpp.o' ! -name 'main.cpp.o' | LC_ALL=C sort)
DEFS=$(sed -n 's/^CXX_DEFINES = //p' "$OBJDIR/flags.make")
INCS=$(sed -n 's/^CXX_INCLUDES = //p' "$OBJDIR/flags.make")
LIBS=$(pkg-config --libs x11 xext xinerama xrandr xft xfixes xrender freetype2)
# shellcheck disable=SC2206
HFLAGS=(-std=c++11 $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS $DEFS $INCS -iquote "$SRC/src"
        "-DHERBSTLUFT_VERSION=\"$(cat VERSION)\"" "-DHERBSTLUFT_GLOBAL_AUTOSTART=\"/etc/xdg/herbstluftwm/autostart\"")

mkdir -p build-fuzz/harness
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o build-fuzz/harness/lsan_off.o
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o build-fuzz/harness/standalone_main.o

# 2) Each harness twice: libFuzzer binary + run-once standalone reproducer.
for h in fuzz_commands fuzz_load fuzz_css fuzz_rules fuzz_keybind fuzz_xclient; do
  $CXX "${HFLAGS[@]}" -c "mayhem/harnesses/$h.cpp" -o "build-fuzz/harness/$h.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE build-fuzz/harness/$h.o "${OBJS[@]}" \
      build-fuzz/harness/lsan_off.o $LIBS -o "/mayhem/$h"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS build-fuzz/harness/$h.o build-fuzz/harness/standalone_main.o \
      "${OBJS[@]}" build-fuzz/harness/lsan_off.o $LIBS -o "/mayhem/$h-standalone"
done

# 3) Normal-flags build for the functional test (mayhem/test.sh).
cmake -S . -B build-test -DWITH_DOCUMENTATION=OFF -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build build-test -j"$MAYHEM_JOBS" --target herbstluftwm herbstclient
