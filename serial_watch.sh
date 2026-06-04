#!/usr/bin/env zsh
# Watch for USB serial port and connect immediately when it appears.
# Matches both CDC (usbmodem, C3/S3) and CH340 (usbserial, WROOM).

PYTHON=~/.platformio/penv/bin/python

echo "Watching for serial port..."
while true; do
    ports=(/dev/cu.usbmodem*(N) /dev/cu.usbserial*(N))
    PORT=${ports[1]}
    if [[ -n "$PORT" ]]; then
        echo "---- Connected to $PORT ----"
        "$PYTHON" - "$PORT" <<'EOF'
import serial, sys, time
port = sys.argv[1]
try:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False
    s.rts = False
    s.open()
    while True:
        data = s.read(256)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
except Exception:
    pass
EOF
        echo ""
        echo "---- Disconnected, watching again... ----"
    fi
    sleep 0.1
done
