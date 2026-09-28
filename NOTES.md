# Implementation notes

## Current control coverage and hardware path

The running remapper supports buttons, directional hats, and absolute axes,
including held modifiers, multiple actions per binding, and cross-controller
routing. It accepts any number of named evdev inputs and uinput joysticks.
Persistent modes and timer-driven tap/hold button bindings are supported.
Additional output types are later milestones.
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
both forms produce the same typed actions. `button` is a one-based index;
`button_code` is an explicit Linux EV_KEY code. `axis` and `hat` use Linux
EV_ABS codes. Indexed physical buttons are resolved from capabilities at
connect time and normalized before reaching the gesture engine. Reconnects
with changed button layouts are rejected. The virtual joystick preset advertises
all 79 button codes (288–303, 704–766), axes 0–7 and hats 16–23. See
[AGENTS/CODING_STANDARDS.md](AGENTS/CODING_STANDARDS.md) for coding conventions.
Code 767 is left out because the currently inspected Wine and SDL evdev paths
skip `KEY_MAX`; the preset's eight axes also avoid Wine's six-axis gamepad
remapping heuristic.

## Verification

`ctest --test-dir build --output-on-failure` runs configuration,
gesture-engine, hat/axis, mode/tap-hold, and output-frame tests plus an end-to-end hardware
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

## Modes and tap/hold

Bindings are indexed across all configured modes. A mode action selects a
persistent mode for future button and hat gestures; already captured gestures
and pending tap/hold deadlines remain attached to their original binding until
release. Axes instead reroute their cached position immediately on a mode
change. A button's `threshold_ms` and optional `tap`/`hold` branches capture
the current binding at press time. The engine uses an injectable `Clock` and
exposes its next deadline to the poll loop; expired timers run serially with
input events. Releasing before the deadline activates tap actions, followed by
their cleanup in a second virtual frame. At the deadline, hold actions
activate while the button is held and clean up at release. A mode action in
either branch persists after release. Device loss and shutdown cancel pending
deadlines without triggering a tap. See [examples/modes.yaml](examples/modes.yaml).
To try it with `evtest`, hold physical button 1, then hold button 5 past 200 ms:
virtual button 1 remains asserted until released, button 8 asserts during the
hold, and new presses of 1 use output 2. Moving physical axis 0 routes to
virtual axis 1 in the alternate mode. Tap button 5 to pulse button 7 in two
output frames; press button 6 to return to the default mode.
The remapper prints `Initial mode: default` on startup and logs persistent
transitions such as `Mode changed: default -> alternate` when they occur.

## Joyful Star Citizen profile

`examples/star_citizen.yaml` ports the two VKB sticks in the Joyful
`sc_profile`. Joyful's zero-based indices are incremented to the profile's
one-based button numbers. The physical numbers resolve against each VKB's
actual EV_KEY capabilities; the virtual outputs use our 79-button joystick
preset. Joyful's momentary Modifier mode is the held left-button-5 layer over
the three persistent modes. The profile retains the USB bus, device names,
vendor and product IDs, but its virtual button ordinal layout is deliberately
not identical to Joyful's. The source Joyful profile had already omitted Gremlin mouse
acceleration and two macros; those omissions remain.

## Live profile reload

`run_hardware` retains the profile path. SIGHUP sets a signal-safe flag;
interactive stdin is polled alongside evdev and timers, with noncanonical
terminal input restored on exit. Both request a reload within the serialized
event loop. Reload parses a candidate profile and constructs its gesture
engine before changing the active mapping. A failed parse, an unsupported
virtual-device shape change, or unavailable capabilities on a still-connected
physical device leaves the old profile running.

Virtual uinput objects and the `OutputFrames` buffer persist across reloads.
Old gestures release their claims and cancel timers. Unchanged physical evdev
backends update their watched controls without dropping their grabs; changed
physical definitions close their old handles and retry new ones if necessary.
Currently held buttons are suppressed until release, and hats are cached as
inert baselines. The new engine starts in `modes.initial` and reasserts the
current position of connected axes. Old-output cleanup and new-axis assertions
are coalesced in a frame, preventing an unnecessary neutral step when an axis
stays on the same destination. Virtual-device additions or changes to names,
bus, IDs, axis ranges or button capabilities require a full restart.

## Mode announcements (text-to-speech)

When configured, mode changes are spoken aloud with espeak-ng. `run_hardware`
receives an optional `Speaker*`; `report_mode_change` calls `speak()` with the
new mode's name after logging the transition, so the announcement covers both
input-driven and reload-driven mode changes. The initial mode is not announced
because the remapper has not switched modes yet.

`Speaker` is a small interface in `include/joystick_penguin/speech.hpp`.
`EspeakSpeaker` implements it with a worker thread: construction starts the
thread, `speak()` only enqueues a phrase and notifies a condition variable, and
the worker owns every espeak-ng call. Initialization, voice selection, and the
speed/pitch/range parameters are applied once on the worker before it drains the
queue, so espeak-ng is never touched from the serialized engine loop. Destroying
the speaker stops the worker and terminates espeak-ng.

The defaults are espeak-ng's default English voice with speed 130, pitch 20, and
range 0. `main` uses CLI11 for `--voice`, `--speed`, `--pitch`, `--range`, and
`--no-tts`, with range validators rejecting values outside espeak-ng's limits
before startup. The dependency is optional: configure with `-DJP_ENABLE_TTS=OFF`
to build without espeak-ng, in which case the tuning flags are rejected with a
clear message. Announcement failures are reported on stderr and never stop the
remapper.
