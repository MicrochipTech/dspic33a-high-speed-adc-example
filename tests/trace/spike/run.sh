#!/bin/sh
# P0.3 spike: generate the fake xc.h, build timebase.c + dma.c UNCHANGED in
# the three C modes and the C++ proxy variant, run each, keep the traces in
# build/trace_spike/. Run from Git Bash:  sh tests/trace/spike/run.sh
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SPK=$ROOT/tests/trace/spike
OUT=$ROOT/build/trace_spike
GEN=$OUT/gen
mkdir -p "$OUT"

python "$ROOT/tools/gen_fake_sfr.py" --out "$GEN" --cxx || exit 1
python "$ROOT/tools/gen_fake_sfr.py" --out "$OUT/gen_macros" --style macros || exit 1

LDFLAGS="-Wl,--disable-dynamicbase"
CFLAGS="-std=c11 -O1 -Wall -Wextra -Werror -mno-ms-bitfields -fno-strict-aliasing -Wno-pointer-to-int-cast"

for MODE in 1 2 3; do
  echo "== C, TRACE_MODE $MODE"
  G=$GEN
  LD=$GEN/sfr_syms.ld
  if [ $MODE -eq 2 ]; then G=$OUT/gen_macros; LD=; fi
  gcc $CFLAGS -DTRACE_MODE=$MODE -I"$SPK" -I"$G" -I"$ROOT" \
      "$SPK/spike_main.c" "$SPK/recorder.c" "$SPK/stubs.c" "$G/sfr_table.c" $LD \
      "$ROOT/timebase.c" "$ROOT/dma.c" $LDFLAGS -o "$OUT/spike_c$MODE.exe" 2> "$OUT/build_c$MODE.log"
  if [ $? -ne 0 ]; then echo "BUILD FAILED:"; grep -m 8 "error" "$OUT/build_c$MODE.log"; continue; fi
  "$OUT/spike_c$MODE.exe" > "$OUT/trace_c$MODE.txt"
  echo "exit $?, $(wc -l < "$OUT/trace_c$MODE.txt") trace lines"
done
if [ -f "$OUT/spike_c3.exe" ]; then
  echo "== C, TRACE_MODE 3 with reads"
  TRACE_READS=1 "$OUT/spike_c3.exe" > "$OUT/trace_c3_reads.txt"
  echo "exit $?, $(wc -l < "$OUT/trace_c3_reads.txt") trace lines"
fi

echo "== polling: clock.c, TRACE_MODE 3, read hooks on / off"
gcc $CFLAGS -DTRACE_MODE=3 -I"$SPK" -I"$GEN" -I"$ROOT" \
    "$SPK/spike_poll.c" "$SPK/recorder.c" "$SPK/stubs.c" "$GEN/sfr_table.c" "$GEN/sfr_syms.ld" \
    "$ROOT/timebase.c" "$ROOT/clock.c" $LDFLAGS -o "$OUT/spike_poll.exe" 2> "$OUT/build_poll.log"
if [ $? -ne 0 ]; then echo "BUILD FAILED:"; grep -m 8 "error" "$OUT/build_poll.log"; else
  "$OUT/spike_poll.exe" > "$OUT/trace_poll.txt"; echo "hooks on:  exit $?, $(tail -3 "$OUT/trace_poll.txt" | head -1)"
  POLL_HOOKS=0 "$OUT/spike_poll.exe" > "$OUT/trace_poll_nohooks.txt"; echo "hooks off: exit $?, $(grep -m1 '^F' "$OUT/trace_poll_nohooks.txt")"
fi

echo "== C++ proxies (approach b), compile only"
CXXFLAGS="-std=c++17 -O1 -Wall -Wextra -fno-strict-aliasing"
for SRC in timebase dma; do
  g++ -x c++ $CXXFLAGS -I"$SPK/cxx" -I"$SPK" -I"$GEN" -I"$ROOT" \
      -c "$ROOT/$SRC.c" -o "$OUT/cxx_$SRC.o" 2> "$OUT/build_cxx_$SRC.log"
  echo "$SRC.c as C++: exit $?, $(grep -c 'error' "$OUT/build_cxx_$SRC.log") error lines"
  grep -m 6 "error" "$OUT/build_cxx_$SRC.log"
done
