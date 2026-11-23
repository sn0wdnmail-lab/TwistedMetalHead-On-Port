#!/bin/bash
# memwatch.sh <seconds>: starts the game with drive.sh and prints its memory use every 5 s; if it
# passes 2.5 GB, saves every thread's stack (gdb) to /tmp/memwatch_bt.txt and stops the game.
D=$(dirname "$0")/drive.sh
$D start > /dev/null
for i in $(seq 1 $(( $1 / 5 ))); do
  sleep 5
  P=$(pgrep -x ps2EntryRunner) || { echo "exited at $((i*5)) s"; break; }
  RSS=$(awk '/VmRSS/{print int($2/1024)}' /proc/$P/status)
  echo "$((i*5)) s: ${RSS} MB"
  if [ "$RSS" -gt 2500 ]; then
    gdb -p "$P" -batch -ex "thread apply all bt 14" > /tmp/memwatch_bt.txt 2>&1
    echo "stacks in /tmp/memwatch_bt.txt"; $D stop > /dev/null; break
  fi
done
