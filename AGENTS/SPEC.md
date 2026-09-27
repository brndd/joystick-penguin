# Joystick Penguin: Project Specification

## 1. Purpose

Joystick Penguin is a Linux joystick remapper written in C++. It reads physical
controllers and creates virtual controllers for games that need stable,
configurable joystick inputs.

The tool must support configurable physical controllers and virtual output
devices without assuming a particular brand, controller count, or game.

The defining interaction is **press-time mapping capture**: holding a control,
pressing a modifier, and then pressing another control must not change or
release the first control's output. Each control retains the mapping selected
when its own gesture began.

## 2. First usable prototype

The first usable release must support:

- Any number of configured physical evdev controllers and uinput virtual
  joysticks, within operating-system limits.
- Configurable exclusive access to each physical evdev controller so games can
  receive remapped input from the virtual device without also receiving raw
  input from the physical device.
- Button, hat, and absolute-axis mappings.
- Bindings from any physical controller to any configured virtual controller.
- Arbitrarily named persistent modes selected by the configuration.
- Held modifiers independent of persistent modes.
- Tap-versus-hold actions, including actions that select a persistent mode.
- Reliable output cleanup on physical release, persistent-mode change, device
  loss, and shutdown; reconnecting a controller restores its mappings.

A small manually authored YAML profile is sufficient for this milestone. TTS,
live configuration reload, combos, mouse outputs, and macros are deferred.

## 3. Input and output architecture

### 3.1 Input backends

The engine consumes normalized events identified by physical device, event
type, and control code. Input decoding is separated from mapping decisions.

Implement **evdev** as the initial backend. The backend must preserve event
ordering and handle synchronization and device loss explicitly. Identify
devices by stable configuration rather than assuming that `/dev/input/eventN`
remains constant. When a controller is unplugged, release its owned outputs,
cancel its pending timers, and clear any modifiers it holds without disrupting
other controllers. Continue running and detect when that controller is
replugged, even if it receives a different event node; reopen it and resume
processing without a restart. Do not replay stale presses or activate a
mapping solely because the controller reappeared. Avoid confusing two devices
that share a model name when matching configured identities.

Support an optional **exclusive evdev grab** per physical controller (Linux
`EVIOCGRAB`). Default to enabled so other evdev consumers do
not also receive that controller's raw events while it is remapped. Allow
users to disable it for devices they intentionally share with other programs.
If an enabled grab fails, report the error clearly and do not silently treat
the controller as exclusively captured. Release the grab on shutdown or
disconnect and reacquire it when the controller is reconnected. This controls
the evdev node; it does not promise to hide other interfaces exposed by the
same hardware.

Leave an interface boundary for a future **HIDRAW** backend. HIDRAW exposes raw
HID reports and may reveal controls unavailable through evdev, but report
decoding, report-to-report state tracking, and device-specific behavior belong
entirely in that backend. It is not part of the first prototype. Do not read
the same physical control from both backends unless duplicate events are
explicitly reconciled.

### 3.2 Virtual devices

Create the configured number of named virtual joysticks through uinput.
Advertise valid capabilities and absolute-axis ranges; hats must use
appropriate ternary ranges. Bindings reference virtual devices by name, not by
output-device creation order. Input-device reconnection must not unnecessarily
recreate virtual devices.

The output layer tracks which actions currently own each virtual button. If
two actions hold the same virtual button, releasing one must not release the
other. Hats and absolute axes require an explicit arbitration and neutral-value
policy rather than button reference counting.

### 3.3 Event processing

Process physical events, timer expirations, mode changes, and output-state
changes in one serialized engine loop. Buffer output events into coherent
frames. Avoid allowing a timer thread or device reader to mutate mapping state
concurrently with the engine.

## 4. Mapping semantics

### 4.1 Persistent modes and held modifiers

A **persistent mode** is a named operating context selected by configuration. A
**modifier** is a physical control whose held state affects the selection of
*new* gestures. Pressing or releasing a modifier is **not** a mode change and
must not, by itself, cancel an existing output.

The engine maintains the persistent mode and the set of held modifiers
separately. A persistent-mode change retains the first prototype's
cancellation policy: active bindings invalid in the new mode are cleaned up
immediately. A physical control still held across that change does not
automatically activate a replacement binding.

A modifier control may also have its own output binding. Select that binding
from the modifier state **before** its own press is added to the held set;
subsequent presses see the newly held modifier. A binding cannot require a
modifier held by its own physical input, since it could never become eligible
on that input's initial press. The modifier's own button output activates on
press; tap-versus-hold behavior is a later milestone.

### 4.2 Gesture capture

When a physical button is initially pressed:

1. Take a consistent snapshot of mode and modifier state.
2. Select the eligible binding according to documented precedence.
3. Record the selected actions against that physical press.
4. Deliver its eventual release to those recorded actions, regardless of
   subsequent modifier changes.

A modifier-specific binding takes precedence over an ordinary binding for the
same physical input and mode. Among eligible bindings, the one requiring the
most held modifiers wins. An ordinary binding is the fallback if no eligible
modified binding exists. Equally specific conflicting bindings must produce a
configuration error rather than depend on YAML order.
Independently intended multiple actions should be expressed as actions in one
binding.

Key-repeat events must not be treated as new presses. Releasing a modifier
must not activate an ordinary mapping for a control that was already held.
Track every physical down, including one with no eligible binding: changing
modifiers while it is held must not retroactively give it an output, even if a
duplicate down event arrives. Only a release followed by a fresh press may
select a different binding.

**Required sequence:**

| Step | Expected result |
| --- | --- |
| Press A | A's ordinary output activates |
| Press Modifier | A's output remains active |
| Press B | B's modified output activates |
| Release Modifier | Both outputs remain active |
| Release A | Only A's output releases |
| Release B | B's output releases |

A newly pressed A while Modifier is held selects A's modified mapping, if one
exists.

### 4.3 Hats and axes

Treat each hat axis as a physical stateful control. A movement from neutral
begins a directional gesture; return to neutral ends it. A direct
direction-to-direction movement must release or update the previous direction
correctly. The two axes of a hat must support diagonals without interfering
with each other.

Absolute axes use **live modifier routing**, deliberately distinct from
captured button and hat gestures. Cache the latest physical position, including
before the first movement. While a modifier is held, an eligible modified axis
binding receives new values; pressing or releasing a modifier immediately
removes this axis's ownership from its old virtual destination and sends the
cached position to the newly selected destination in one coherent output frame.
When no destination is selected, remove ownership without asserting a new one.
This policy also applies to non-centering throttles: they need no assumed
return-to-zero point to change destinations. On initial connection or
reconnection, cache physical axis positions without asserting outputs solely
because the device appeared; the next physical axis or modifier event may
activate routing.

Virtual absolute outputs must declare valid ranges and neutral values. Hats
use a ternary range and zero neutral. If several physical actions drive the
same virtual absolute control, the last updated active owner wins; removing
that owner restores the most recently updated remaining owner's value, or the
configured neutral value when none remain. This applies independently to each
hat axis so diagonal movement is possible without interference.

### 4.4 Tap and hold

A tap/hold binding is selected when its physical button is pressed. Its
threshold timer and resulting actions belong to that captured gesture, even
if modifier state changes before the threshold or release. Mode-changing
actions remain persistent-mode changes and apply the normal mode-transition
cleanup policy.

Use a controllable clock in tests so behavior at, before, and after the
threshold is deterministic.

### 4.5 Cleanup

Every action must support the cleanup appropriate to its output. Cleanup is
required on physical release, applicable persistent-mode change, device loss,
and orderly shutdown. It must not emit a release for an output the action
never asserted.

Live reload is deferred, but the engine's state model must permit a future
reload to cancel active gestures and clear output ownership before replacing
bindings.

## 5. Configuration format

Use a versioned, declarative YAML document with named devices, modifier
definitions, and bindings. The precise spelling may evolve during the prototype;
the following shows the intended concepts:

```yaml
version: 1

devices:
  controller_a:
    kind: evdev
    path: /dev/input/by-id/example-controller-a-event-joystick
    grab: true
  controller_b:
    kind: evdev
    path: /dev/input/by-id/example-controller-b-event-joystick
    grab: false
  virtual_a:
    kind: uinput
    preset: joystick
  virtual_b:
    kind: uinput
    preset: joystick

modes:
  initial: default
  names: [default, alternate]

modifiers:
  shift:
    input: {device: controller_a, button: 4}

bindings:
  - input: {device: controller_a, button: 5}
    modes: [default, alternate]
    action: {type: button, device: virtual_b, button: 10}

  - input: {device: controller_a, button: 5}
    modes: [default, alternate]
    modifiers: [shift]
    action: {type: button, device: virtual_b, button: 40}
```

The two controllers in this example illustrate cross-device routing; neither
their number nor their names are built-in limits.

For one action, `action: {...}` is shorthand. To assert several independent
outputs from a single captured gesture, use a nonempty `actions: [{...}, {...}]`
list instead. A binding must specify exactly one of `action` or `actions`.
Axis input codes and virtual output axis codes use Linux `EV_ABS` numbers.
Directional hat inputs identify the hat component and its sign, while hat
outputs advertise `ABS_HAT*` axes with the ternary range. Non-hat virtual
axis outputs declare a minimum, maximum, and neutral value, for example:

```yaml
devices:
  virtual_stick:
    kind: uinput
    preset: joystick
    axes:
      0: {min: -32768, max: 32767, neutral: 0}
      2: {min: 0, max: 255, neutral: 0}
```

See `examples/controls.yaml` for complete physical hat and axis bindings.

The configuration loader must validate device references, modifier names,
input and output capabilities, mode references, and ambiguous binding
precedence before starting the engine. Document whether numeric button values
are Linux event codes or logical indices.

The engine should consume validated, typed objects rather than making
YAML-specific decisions during event processing.

## 6. Joystick Gremlin conversion: later milestone

Joystick Gremlin is a Windows application written in Python that serves
a similar purpose to Joystick Penguin. As Joystick Gremlin is widely used
and profiles for it are often shared by users to share premade configurations
online, down the line we want to be able to convert Joystick Gremlin profiles
to our format.

The converter is not required for the first usable prototype; a manually
authored profile can be used initially.

Joystick Gremlin is open source under GPLv3. A future converter can examine
its implementation and parse its profile format, while keeping
Gremlin-specific concepts out of the Joystick Penguin runtime.

Use this pipeline:

**Gremlin XML → parsed Gremlin model → Joystick Penguin
devices/modes/modifiers/bindings/actions → YAML.**

The converter should make source-to-output decisions explicit, including
inherited modes, temporary mode actions that represent modifiers, tap/hold
behavior, axis polarity and ordering, and one-based vJoy button numbers.
Unsupported actions must produce actionable warnings rather than silently
disappear.

Validate conversion against representative Gremlin XML profiles and
deterministic output fixtures.

## 7. Delivery sequence

1. **Project skeleton:** C++ build, GPL-3.0-or-later license, YAML loading,
   deterministic tests, and input/output interfaces. Retain appropriate
   notices and attribution if code is copied from another project.
2. **Hardware path:** read one physical evdev control and emit it through one
   uinput device; then support any number of configured input and output
   devices, including configurable exclusive grabs and unplug/replug recovery.
3. **Gesture engine:** implement binding selection, modifier state, press
   capture, release routing, and virtual-button ownership. Prove the
   A–Modifier–B sequence.
4. **Control coverage:** implement hats, diagonals, axes, cross-controller
   outputs, and valid virtual capabilities.
5. **Modes and tap/hold:** implement persistent-mode transitions, timer-driven
   actions, and cleanup.
6. **Usable manual profile:** configure representative controls and verify
   mappings across different physical and virtual devices.
7. **Later milestones:** add uinput virtual keyboard, mouse, and gamepad outputs
   alongside joysticks; expand rule types, add reload and TTS if needed,
   investigate HIDRAW against real hardware, and build the Gremlin converter.

   Configure these as named virtual devices with appropriate capabilities:
   keyboard keys, mouse buttons and relative movement/scrolling, and gamepad
   buttons and absolute axes. Gamepads and joysticks share the uinput mechanism
   but should advertise capabilities and identity suitable for their device
   type. Track ownership and cleanup for held keys and mouse buttons as for
   joystick buttons; relative mouse movement is a transient output.

## 8. Verification and acceptance

Automated tests must cover modifier ordering, release ordering, overlapping
virtual-button ownership, key repeats, timer thresholds, hat direction
changes and diagonals, mode-transition cancellation, cross-controller
routing, and device-loss cleanup.

Verify that an enabled exclusive grab prevents another evdev consumer from
receiving raw events, that disabling it permits shared input, and that a
failed grab is reported. After reconnecting a grabbed controller, verify the
grab is established again.

Test disconnect and reconnect of one controller while other controllers remain
active, including while a button or modifier is held. Verify outputs from the
lost controller are cleared, unaffected controllers keep working, and mappings
resume when the original controller reappears under a new event node.

Manual validation must use physical controllers and inspect the generated
virtual events. Check virtual-axis and hat capabilities in Linux, verify
cross-device routing, and exercise unplug/replug without restarting the tool.

**The first prototype is accepted when** the real A–Modifier–B sequence
retains both intended outputs until their respective physical releases;
persistent-mode and tap/hold controls work; any number of configured
controllers can be mapped; disconnected devices recover after reconnection;
and no tested transition leaves a virtual input stuck.
