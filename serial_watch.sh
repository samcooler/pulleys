#!/bin/bash
# Watch for USB serial port and connect immediately when it appears
PORT_PATTERN="/dev/cu.usbmodem*"

echo "Watching for serial port ($PORT_PATTERN)..."
while true; do
    PORT=$(ls $PORT_PATTERN 2>/dev/null | head -1)
    if [[ -n "$PORT" ]]; then
        echo "---- Connected to $PORT ----"
        cat "$PORT" 2>/dev/null
        echo ""
        echo "---- Disconnected, watching again... ----"
    fi
    sleep 0.1
done
