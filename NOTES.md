# Implementation notes

## Current control coverage and hardware path

The running remapper supports buttons, directional hats, and absolute axes,
including held modifiers, multiple actions per binding, and cross-controller
routing. It accepts any number of named evdev inputs and uinput joysticks.
Persistent mode changes, tap/hold, and additional output types are later
milestones; runtime profiles currently require one persistent mode.
Profiles containing unsupported runtime features are rejected rather than
silently partially applied. `--check` only checks the versioned YAML profile;
it does not open devices or check their capabilities.

## Gesture engine architecture

`GestureEngine` consumes validated `Config` and ordered, normalized
`InputEvent`s. It owns mapping state; libevdev decodes physical events, while
the uinput output sink writes the resulting `OutputEvent`s. Neither backend
selects bindings. The hardware loop calls the engine serially and collects
output changes into frames.

At construction, the engine indexes bindings by physical device, control kind,
code, and (for hats) direction. It keeps the persistent mode and held modifier
names separately. On a fresh button press or hat direction, it selects the
eligible binding requiring the most held modifiers and records its index in a
`Gesture`. An `OutputClaim` records each asserted virtual control and the owner
ID needed to release or update it. Button presses are recorded even without a binding:
later modifier changes and duplicate downs cannot activate a control already
held. A modifier's own gesture is selected before adding that modifier to the
held set. Releasing a button or ending a hat direction cleans up its recorded
actions without selecting a replacement binding.

Absolute axes take a different path. Each physical axis keeps its latest value
and range plus its current selected binding. A new axis value updates its
virtual destination. A modifier change reselects the axis binding immediately,
removes ownership of the old destination, and routes the cached value to the
new one. Connection and resync baselines cache values without asserting
outputs. Hat X and Y remain separate directional gestures, so diagonals do
not interfere with each other.

Every asserted output has an owner. Buttons use owner counts: only the first
owner emits a press and the last emits a release. Absolute outputs store each
owner's latest value and update order; the most recently updated owner wins.
Removing it restores the next owner or the configured neutral. The `OutputChanges`
accumulator keeps only the final transition per output within an engine event;
`OutputFrames` coalesces those transitions across a physical `SYN_REPORT`
frame before sending them to uinput. On device loss or sync loss, the engine
removes that device's gestures, axes, and held modifiers; surviving captured
gestures retain their actions, while surviving live axes may reroute. Shutdown
removes every owner.

## Input and output backends

`libevdev` reads physical events and handles `SYN_DROPPED` synchronization;
libevdev's uinput API creates the virtual devices and writes output frames.
The input backend, frame-based output sink, and clock interfaces are in
`include/joystick_penguin/io.hpp`.

Each physical hat component (an `ABS_HAT*` code) has independent directional
gestures: returning to zero ends one, and changing directly from `-1` to `+1`
ends one and begins the other. Diagonals combine X and Y without either axis
ending the other's gesture. Hat directions can drive virtual buttons or hats;
buttons can also hold a virtual hat direction. Absolute-axis bindings drive
virtual axes, optionally inverted. Modifier-dependent axes switch destinations
*live*: modifier changes neutralize the old target and send the cached current
position to the newly selected target in the same output frame. This differs
deliberately from captured button and hat bindings. Axis inputs cannot drive
buttons until a threshold policy is defined.

Virtual non-hat axes declare `min`, `max`, and `neutral` under their device's
`axes` map. Physical ranges come from libevdev on connection; positions are
linearly scaled and clamped into the virtual range. Hat capabilities are
automatically ternary (`-1..1`, neutral `0`). When several actions drive the
same virtual axis or hat component, the last updated active owner wins. On its
release, the most recently updated remaining owner resumes, or the configured
neutral value is emitted. The output frame buffer coalesces changes to one
final value per control before `SYN_REPORT`.

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
suppressed until physically released. Hat directions and axis positions are
cached as baselines without asserting outputs merely because a device appeared.
An axis update or modifier change may route the cached position after that.

The YAML loader validates names, kinds, codes, ranges, references, unreachable
self-modified bindings, and ambiguous binding precedence before returning a
typed `Config`. A single `action` is shorthand for a nonempty `actions` list;
both forms produce the same typed actions. Numeric `button` and
`axis` values are Linux event codes (for example, `BTN_SOUTH` is 304 and
`ABS_X` is 0), not logical button indices. See
[AGENTS/CODING_STANDARDS.md](AGENTS/CODING_STANDARDS.md) for coding conventions.

## Verification

`ctest --test-dir build --output-on-failure` runs configuration,
gesture-engine, hat/axis, and output-frame tests plus an end-to-end hardware
test using synthetic evdev devices. CTest marks that last test skipped if
`/dev/uinput` is unwritable or the generated `/dev/input/eventN` nodes are
unreadable. Where suitable, it can
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

For control coverage, edit the two stable physical paths in
[examples/controls.yaml](examples/controls.yaml), inspect both virtual nodes
with `evtest`, move sticks and throttle, switch Shift while the stick is held,
and move the hat around all four diagonals. Check the advertised virtual
axis and hat ranges as well as the resulting events.
