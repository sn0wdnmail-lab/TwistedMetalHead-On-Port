#!/bin/bash
# Drives the Linux build under a virtual display, for unattended tests.
#   drive.sh start              start the game (R=vulkan|software, EXTRA="VAR=value ...")
#   drive.sh key <key> [secs]   hold a key (xdotool key names: x, Return, Down, F2, ...)
#   drive.sh shot <name>        screenshot of the game window into $SHOTS
#   drive.sh stop
#   drive.sh seq "x:30" "Return:8" ...   press each key, then wait that many seconds
# Paths: TMHO_BIN (the built ps2EntryRunner), TMHO_ELF (your SCUS_976.21), TMHO_ISO (your disc image).
# Needs Xvfb, xdotool and ImageMagick; the Vulkan renderer runs on lavapipe there (slow).
export DISPLAY=${DISPLAY_NUM:-:99}
BIN=${TMHO_BIN:?set TMHO_BIN}
LOGS=${LOGS:-/tmp/tmho_logs}
SHOTS=${SHOTS:-/tmp/tmho_shots}
case "$1" in
start)
    pgrep -x Xvfb > /dev/null || { (nohup Xvfb "$DISPLAY" -screen 0 1920x1080x24 > /tmp/xvfb.log 2>&1 &); sleep 2; }
    pkill -x ps2EntryRunner; sleep 1
    rm -rf "$LOGS" "$SHOTS"; mkdir -p "$LOGS" "$SHOTS" /tmp/tmho_saves
    (env PS2X_RENDERER=${R:-software} PS2X_NO_AUDIO=1 PS2X_LOG_DIR="$LOGS" $EXTRA \
        setsid nohup timeout 1500 "$BIN" "${TMHO_ELF:?set TMHO_ELF}" --iso "${TMHO_ISO:?set TMHO_ISO}" \
        --saves /tmp/tmho_saves > /tmp/tmho_run.log 2>&1 &)
    sleep 3; echo started ;;
key)
    WID=$(xdotool search --name "Twisted Metal" | head -1)
    xdotool windowactivate --sync "$WID" 2>/dev/null; xdotool windowfocus "$WID" 2>/dev/null
    xdotool keydown "$2"; sleep "${3:-0.25}"; xdotool keyup "$2"; echo "pressed $2" ;;
shot)
    WID=$(xdotool search --name "Twisted Metal" | head -1)
    import -window "$WID" "$SHOTS/$2.png" 2>/dev/null && echo "$SHOTS/$2.png" ;;
stop)
    pkill -x ps2EntryRunner; echo stopped ;;
seq)
    shift
    for step in "$@"; do k=${step%%:*}; w=${step##*:}; "$0" key "$k" 0.3 > /dev/null; sleep "$w"; done
    echo "seq done" ;;
esac
