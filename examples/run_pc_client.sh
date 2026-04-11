#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
BINARY="$BUILD_DIR/examples/pc_client"
LOG="/tmp/pc_client.log"
SIGNAL_URL="${1:-ws://localhost:8080/signal}"

if [ ! -f "$BINARY" ]; then
    echo "Binary not found: $BINARY"
    exit 1
fi

# Kill any existing pc_client process
pkill -f "pc_client" 2>/dev/null && echo "Killed existing pc_client"

echo "Starting pc_client -> $SIGNAL_URL"
echo "Log: $LOG"

"$BINARY" "$SIGNAL_URL" </dev/null >"$LOG" 2>&1 &
PID=$!
echo "PID: $PID"

sleep 1
if kill -0 "$PID" 2>/dev/null; then
    echo "Running. Tailing log (Ctrl+C to stop tailing, process keeps running):"
    tail -f "$LOG"
else
    echo "Process exited. Log:"
    cat "$LOG"
fi
