#!/usr/bin/env zsh
# Watch for USB serial port and connect immediately when it appears.
# Matches both CDC (usbmodem, C3/S3) and CH340 (usbserial, WROOM).
#
# Two-way: what the board prints comes out here, and what you type goes to the
# board a line at a time. Every role takes commands on serial -- "?" for its ID,
# and per-role bench commands (see handleSerial in each src/<role>/main.cpp) --
# and a watcher that only listened made those look broken rather than unsent.

PYTHON=~/.platformio/penv/bin/python

echo "Watching for serial port..."
while true; do
    ports=(/dev/cu.usbmodem*(N) /dev/cu.usbserial*(N))
    PORT=${ports[1]}
    if [[ -n "$PORT" ]]; then
        echo "---- Connected to $PORT (type a command + Enter; Ctrl-C to quit) ----"
        "$PYTHON" - "$PORT" <<'EOF'
import serial, sys, select, time
port = sys.argv[1]
try:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0
    # The CH340 drives the auto-reset circuit from DTR/RTS, so asserting either
    # on open holds the chip in reset and prints nothing. See [env:screen].
    s.dtr = False
    s.rts = False
    s.open()
    while True:
        # Block on either direction. stdin stays line-buffered on purpose: the
        # terminal echoes what you type and you can fix a typo before Enter.
        ready, _, _ = select.select([s.fileno(), sys.stdin], [], [], 0.5)
        if s.fileno() in ready:
            data = s.read(256)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
        if sys.stdin in ready:
            line = sys.stdin.readline()
            if not line:                  # stdin closed -- keep watching output
                continue
            s.write(line.rstrip("\n").encode() + b"\n")
            s.flush()
except Exception:
    pass
EOF
        echo ""
        echo "---- Disconnected, watching again... ----"
    fi
    sleep 0.1
done
