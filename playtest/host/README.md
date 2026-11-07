# The arbiter's game engine, on this computer

`src/arbiter_mesh/gamestate.cpp` and `components.cpp`, built for the host
against stand-ins for Arduino, the mesh and the palette (`shim/`, `stubs.cpp`),
and driven over stdin by `driver.cpp`. `playtest/tests/test_conformance.py`
builds it and plays the same scenarios through it and through
`playtest/engine.py`, so the two engines are checked against each other
without a board. It needs a C++17 compiler and ArduinoJson, which a
`pio run -e arbiter_mesh` leaves in `.pio/libdeps/`; without them the test
skips.

Protocol, one command a line, each answered by its output then a line `.`:

    load <game.json>      reset everything and load a game (from tools/game_to_json.py)
    pull <ch>             a pull on a rope channel
    wait <ms>             let time pass, in 10 ms loop passes
    input <name>          an event by name
    param <name> <value>  set a param live: "ok" or "no"
    state                 "state <name>"
    goals                 "goals <colour> ..."

Unprompted lines: `cue <name> <0|1 enter>`, `info <telemetry>`, `set <k>=<v>`.
