# Implementation notes

## Current gesture engine and hardware path

The running remapper supports button-to-button bindings, including held
modifiers, multiple actions per binding, and cross-controller routing. It
accepts any number of named evdev inputs and uinput virtual joysticks.
Persistent mode changes, tap/hold, hats, axes, and additional output types are
later milestones; runtime profiles currently require one persistent mode.
Profiles containing unsupported runtime features are rejected rather than
silently partially applied. `--check` only checks the versioned YAML profile;
it does not open devices or check their capabilities.

`libevdev` reads physical events and handles `SYN_DROPPED` synchronization;
libevdev's uinput API creates the virtual devices and writes output frames.
`src/gesture_engine.cpp` captures actions at each physical press, tracks held
modifiers separately, and reference-counts virtual-button ownership. A
modifier with its own action selects that action before becoming held. Every
physical down is tracked, even when unmapped, so changing modifiers cannot
activate a control already held. Device loss clears only that device's held
modifiers and actions; captured actions on other devices remain active. The
input backend, frame-based output sink, and clock interfaces are in
`include/joystick_penguin/io.hpp`; processing stays in one serialized loop.

Physical devices are identified by configured stable paths (prefer `/dev/input/by-id/`
or `/dev/input/by-path/`), not by mutable `eventN` numbers or model names.
Direct `eventN` paths are rejected. Use distinct links for distinct physical
devices. On connection, the backend checks that mapped input codes exist;
virtual devices advertise their configured output button codes. `grab` defaults
to true. A failed exclusive grab is reported and retried, never treated as a
successful nonexclusive connection. Setting `grab: false` allows sharing but
cannot bypass another program's exclusive grab.

The loop retries absent/disconnected inputs every half second without
recreating virtual outputs. On sync loss, unplug, or shutdown, it releases
buttons owned by the affected input while preserving other inputs' ownership.
On initial connection and resynchronization, buttons already held are
suppressed until physically released: a reconnect does not create a new press
from stale state. Only a subsequent press can assert an output.

The YAML loader validates names, kinds, codes, references, unreachable
self-modified bindings, and ambiguous binding precedence before returning a
typed `Config`. A single `action` is shorthand for a nonempty `actions` list;
both forms produce the same typed actions. Numeric `button` and
`axis` values are Linux event codes (for example, `BTN_SOUTH` is 304 and
`ABS_X` is 0), not logical button indices. See
[AGENTS/CODING_STANDARDS.md](AGENTS/CODING_STANDARDS.md) for coding conventions.

## Verification

`ctest --test-dir build --output-on-failure` runs configuration and
gesture-engine tests plus an end-to-end hardware test using synthetic evdev
devices. CTest marks that last test skipped if `/dev/uinput` is unwritable or
the generated `/dev/input/eventN` nodes are unreadable. Where suitable, it can
be run separately with the needed permissions using
`sudo ./build/hardware_path_tests`.

For a manual check, run [examples/hardware.yaml](examples/hardware.yaml) with
a real stable device path, observe the printed virtual node with `evtest`,
and press/release its configured button. Unplugging while held should clear
the virtual button; after replugging, a new press should work without restarting
the remapper. The virtual device stays available throughout input reconnection.
To verify press-time mapping capture, use
[examples/gestures.yaml](examples/gestures.yaml): press A, press Shift, press B,
then release Shift, A, and B in that order. Each output must stay active until
its own input is released.
