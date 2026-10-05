#!/usr/bin/env bash
# usage: scripts/demo.sh [SECONDS]   (default 10; BUILD=dir and RATE=packets/s override)
# mdbus_exchange_sim multicasts L3 events on loopback, mdbus_feed_handler keeps the book and
# writes the bus, mdbus_watch shows it once a second. On exit, Ctrl-C included, all stop and the
# bus is removed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${BUILD:-$root/build}"
run_seconds="${1:-10}"
rate="${RATE:-2000}"
bus="demo$$"
log_dir="$(mktemp -d)"
# A multicast group and port of its own per run, as the e2e test does, so two demos never
# cross.
# - Group 239.255.X.Y takes X and Y from the two low bytes of this shell's pid.
# - Port is 20000 plus the pid mod 10000, so it stays in 20000..29999.
# - Both ends derive the instruments from the seed and the instrument count.
group_and_port_args=(
  --group "239.255.$((($$ >> 8) & 255)).$(($$ & 255))" --port $((20000 + $$ % 10000))
  --seed 1 --instruments 64)

cleanup() {
  trap - EXIT INT TERM
  set +e  # every step runs, whatever failed before
  kill $(jobs -p) 2>/dev/null  # this script's background processes
  wait 2>/dev/null
  for program in feed_handler exchange_sim; do
    [ -s "$log_dir/$program.log" ] && sed "s/^/[mdbus_$program] /" "$log_dir/$program.log"
  done
  [ -x "$build/mdbus_watch" ] && "$build/mdbus_watch" --bus "$bus" --destroy
  rm -rf "$log_dir"
}
trap cleanup EXIT
trap 'exit 130' INT TERM  # 130 = 128 + SIGINT, the shell's code for Ctrl-C; the EXIT trap cleans up

echo "building in $build"
if [ ! -f "$build/CMakeCache.txt" ]; then
  if ! cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release >"$log_dir/build.log" 2>&1; then
    tail -30 "$log_dir/build.log"
    exit 1
  fi
fi
targets=(mdbus_exchange_sim mdbus_feed_handler mdbus_watch)
if ! cmake --build "$build" -j --target "${targets[@]}" >>"$log_dir/build.log" 2>&1; then
  tail -30 "$log_dir/build.log"
  exit 1
fi
# "ready" comes once the feed has joined the group: a packet sent before would open with a gap.
# Its --max-ms gives 6 s more than the run: startup, the extra second of traffic, and shutdown.
"$build/mdbus_feed_handler" --bus "$bus" --max-ms $(((run_seconds + 6) * 1000)) \
  "${group_and_port_args[@]}" >"$log_dir/feed_handler.log" 2>&1 &
for _ in $(seq 200); do  # up to 10 s
  grep -q '^ready' "$log_dir/feed_handler.log" && break
  sleep 0.05
done
if ! grep -q '^ready' "$log_dir/feed_handler.log"; then
  echo "demo: the feed handler did not start"
  exit 1
fi
# Traffic outlasts the watcher by a second; at its end of stream the feed leaves, and both print.
# --rate is packets per second and a packet carries up to 10 events, hence rate * 10 per second.
"$build/mdbus_exchange_sim" --events $((rate * 10 * (run_seconds + 1))) --rate "$rate" \
  "${group_and_port_args[@]}" >"$log_dir/exchange_sim.log" 2>&1 &
"$build/mdbus_watch" --bus "$bus" --seconds "$run_seconds"
wait
