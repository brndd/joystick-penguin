# Joystick Penguin

Linux virtual joystick remapper inspired by Joystick Gremlin and Joyful.
The current prototype maps physical joystick buttons, hats, and absolute axes
to virtual joysticks, including held modifiers, persistent modes, and tap/hold
buttons.

## Build

Requires Linux, a C++23 compiler, CMake 3.20+, pkg-config, and libevdev
development headers (Fedora: `libevdev-devel`). CMake downloads yaml-cpp on
first configure and finds CLI11 (Fedora: `cli11-devel`). Text-to-speech
additionally needs espeak-ng development headers (Fedora: `espeak-ng-devel`);
configure with `-DJP_ENABLE_TTS=OFF` to build without that dependency.
The profile editor requires Qt 6 Widgets development files (Fedora: `qt6-qtbase-devel`).
Configure with `-DJP_BUILD_GUI=OFF` for a CLI-only build without Qt.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Try it

Edit [examples/hardware.yaml](examples/hardware.yaml): set `physical.path` to
your joystick's `/dev/input/by-id/` or `/dev/input/by-path/` link. `button: 1`
means its first advertised joystick button. Buttons are **one-based indices**
in profiles; use `button_code: 704` for a literal EV_KEY code if needed.
[examples/gestures.yaml](examples/gestures.yaml) demonstrates modifier capture
and multiple outputs. [examples/controls.yaml](examples/controls.yaml) maps
axes and diagonal hats across two controllers. [examples/modes.yaml](examples/modes.yaml)
demonstrates mode changes and tap/hold buttons. Edit their physical paths to
try them. [examples/star_citizen.yaml](examples/star_citizen.yaml) ports the
two-stick profile from Joyful using one-based physical and virtual button
numbers. The joystick preset advertises all 79 buttons and four hats on each
virtual device. Check the stable paths against your devices.

```sh
./build/joystick-penguin examples/hardware.yaml
```

The program prints the virtual `/dev/input/eventN` path. Use
`evtest <virtual-event-node>` in another terminal to see mapped presses and
releases. Stop the remapper with Ctrl+C. You need read access to the physical
device and virtual event node, and write access to `/dev/uinput`. If a grab
fails, close any other remapper using that physical device.

## Reload a profile

Save changes to the same profile file, then press **r** in the remapper's
terminal (no Enter needed) or send `kill -HUP <remapper-pid>` from another
terminal. Single-key input is enabled automatically when stdin is a terminal;
its settings are restored when the remapper exits. The console reports a
successful reload or explains why it was rejected. An invalid profile leaves
the running mappings active.

Reload keeps the existing virtual joysticks and their event nodes alive. You
may change bindings, modes, modifiers, timers, and physical controllers. New or
repointed physical controllers that are absent will be retried. Changes to
virtual devices' names, identity, advertised controls, or axis ranges require
a restart. Successful reload starts in the profile's initial mode, releases
held virtual buttons and hats, cancels pending taps, and resumes axes at their
current positions. Release and press any physically held button again to
activate its new mapping.

Use `./build/joystick-penguin --check <profile.yaml>` to validate a profile
without opening devices. A mode action is `{type: mode, mode: alternate}`.
Button bindings may specify a positive `threshold_ms` and `tap` and/or `hold`
branches, each containing an `action` or `actions` list. A short release pulses
tap outputs, which stay asserted for `tap_ms` milliseconds (default 50) before
releasing so consumers can register the tap; reaching the threshold activates
hold outputs, which release with the physical button. Mode selections persist
until another mode action changes them. Captured buttons and hats stay held
across mode changes; axes reroute live.

## Profile editing foundation

The C++ profile API in `joystick_penguin/config.hpp` provides
`serialize_config`, `validate_edited_config`, and `save_config_file` for
editors. Validation serializes the edited `Config`, reloads it using the same
rules as `--check`, and rejects values that cannot round-trip. Errors include
paths such as `bindings[3].input` or `modes.initial` for display beside edits.
Saving also reloads the temporary file before replacing the destination.

**Save policy:** Profiles are written from the typed model as version 1 YAML.
Comments, original layout, quoting, numeric notation, and anchors are not
preserved; aliases are expanded. Keep a separate copy if those details matter.
Saves use a temporary file in the destination directory, flush it, and rename
it over the profile only after validation. Failed validation or writing leaves
the original file in place. Existing file permissions are retained.

## Visual profile editor

Run `./build/joystick-penguin-gui [profile.yaml]` to create or edit a profile.
The toolbar provides New/Open/Save/Save As and Undo/Redo. Standard file shortcuts
work. The document name and `*` identify unsaved changes. Editing is live: there
is no Done/Cancel step, and consecutive edits to one mapping collapse into a
single undo step.

- **Mappings:** choose a controller and select one or more physical controls in
  the browser (Ctrl/Shift click, Ctrl+A). The mappings view lists the selected
  controls' mappings; with nothing selected it is empty. Configured controls are
  listed even when hardware is absent. Mapping counts and modifier-only controls
  are shown. Use **Add**, **Duplicate**, and **Delete** to manage mappings.
- Open a mapping to edit its conditions and actions inline. Action forms write
  through as you edit them. Click an action in the left-hand list to edit it on
  the right; add actions below the list and reorder or remove them using the
  buttons beside each action. In Tap/Hold mappings, the combined **Name | Type**
  list shows both branches; use the Type selector on a row to move an action
  between Tap and Hold. **Undo** reverses a
  mapping edit; **Save** writes YAML and does not apply a profile to a running
  remapper.
- **Devices:** edit controllers and virtual joysticks in place. Choose detected
  hardware by name or enter a stable path manually. Connection details contain
  **Exclusive mode** (take the controller exclusively while remapping so other
  applications must use the virtual joystick); the always-visible advanced
  virtual properties contain bus/IDs and axis ranges. The bus choice changes the
  device identity reported to applications (**virtual** or **USB**), not how
  events are delivered. The joystick preset includes 79 buttons, four
  hats, and axes 0–7; extra axes may be added and preset ranges reset.
  When creating a virtual joystick, choose a configured controller to copy
  readable axis ranges and optionally mirror its indexed button labels (1–79).
  An offline source still supplies its saved labels and uses preset axis ranges.
  **Refresh from controller…** on an existing virtual joystick updates matching
  axis ranges and replaces its preset button labels from a readable configured
  controller, retaining virtual-only axes. Changed axis ranges require a
  remapper restart.
- **Modes & modifiers:** edit persistent modes and named held buttons, with a
  table of their mappings; click a row to jump to that mapping. Use the star
  beside a mode to set the **default mode** (the initial mode when remapping
  starts). Use **+** by a section heading to add an item and its trash button
  to remove it with confirmation. Renames update references. Used definitions
  must be removed from mappings before deleting them.
  Returning from a mapping retains the selected mode's or modifier's usage-table
  scroll position.
- **Issues:** profile diagnostics link to the relevant mappings, including both
  sides of a precedence conflict. The status bar at the bottom of the window
  reports validity; when it reports issues, click it to open **Issues**.

**New** starts empty: the Mappings view asks for a physical and a virtual
controller, with buttons that jump to **Devices** and create one. There is no
default dummy device. A virtual joystick is created with preset defaults.

Every listed control has a passive activity indicator. Readable controllers are
monitored without an exclusive grab: buttons show held/released state, axes show
values with a short motion highlight, and hats show their component's sign.
Display updates are throttled, and small axis noise does not keep the motion
indicator illuminated. A one-word monitor status sits at the bottom of the
browser (green **Monitoring**, otherwise a red error) with a refresh button
tooltip **Reconnect monitor**. Another process's exclusive grab can suppress live
events; silence alone does not establish that a grab exists. Static capabilities
and offline editing remain available. Hardware availability is separate from
profile validation.

### Physical input labels (version 1)

Double-click a control's name in the browser to edit its label inline (an empty
label removes it); a name such as `Trigger` keeps the underlying input visible.
Labels apply to all mappings and modifier references for the exact input. They
are optional metadata and do not change runtime behavior:

```yaml
version: 1
# devices, modes, modifiers and bindings as usual
input_labels:
  - input: {device: physical, button: 1}
    label: Trigger
  - input: {device: physical, button_code: 288}
    label: Literal trigger
  - input: {device: physical, axis: 0}
    label: Roll
  - input: {device: physical, hat: {axis: 16, direction: -1}}
    label: Trim negative
```

Each device/kind/code/direction identity may have at most one nonempty label.
Indexed buttons and literal EV_KEY codes remain distinct identities, even if
hardware resolves them to the same button. Different controls may share display
labels. Labels must reference a declared physical device and a valid input
identity, but need not reference a mapping or currently connected hardware.
Removing a mapping retains its label; clearing the label removes the metadata.
Renaming a device updates label references, and removing an unused device removes
its labels. Old profiles load with no labels; the serializer omits `input_labels`
when empty. The profile version remains 1.

Virtual preset buttons can also be labeled by double-clicking their names in
the Mappings browser. Labels appear in output choices, summaries, and search;
the button numbers and emitted events do not change. They are stored separately
from physical input labels:

```yaml
output_labels:
  - {device: virtual, button: 1, label: Trigger}
```

Each virtual device/button pair has at most one label. Renaming or removing a
virtual device updates or removes its labels. Mirroring and refresh replace
labels for that virtual device's preset button slots; labels on other virtual
devices are unaffected.

Saving rewrites YAML according to the policy above. Confirm a saved profile with
`./build/joystick-penguin --check profile.yaml`.

The GUI's `ProfileDocument` owns the editable `Config`, undo history, saved state,
and cached issues. `EditorWindow` handles files, toolbar commands, issues, and
navigation. `MappingWorkspace` owns the control browser, binding model/filter,
and `BindingDetail`; the detail owns `ActionList`, which creates an `ActionEditor`
for the selected action. `SetupWorkspace` owns the device/mode/modifier lists and
usage navigation, creating a focused property form for each selection. Both
workspaces propose edits through the document, which notifies the binding model
of insertions, removals, updates, and replacements. GUI tests cover
offline and mixed-profile editing, labels, draft cancellation, undo/redo, issues,
and `--check` of saved copies of every example. The `control_browser` integration
test uses a synthetic uinput controller to verify held buttons, axis/hat values,
exclusive grabs, and unplugging (skipped when uinput or evdev access is unavailable).
For visual review, `JP_GUI_SCREENSHOT=/tmp/editor.png` captures the 1024×768 fixture
when running `gui_editor_tests`; `JP_GUI_DARK=1` selects its dark-palette fixture,
and `QT_SCALE_FACTOR=2` exercises high-DPI rendering.

## Mode announcements

When built with espeak-ng support, the remapper speaks the name of the new mode
when a mode action changes it. Speech runs on its own thread, so synthesis
never blocks input handling. The voice and audio parameters can be set on the
command line:

```sh
./build/joystick-penguin --voice en --speed 130 --pitch 20 --range 0 profile.yaml
```

The defaults are espeak-ng's default English voice at speed 130, pitch 20, and
range 0 (monotone). `--speed` accepts 80-450 words per minute; `--pitch` and
`--range` accept 0-100. Use `--no-tts` to disable announcements, or configure
with `-DJP_ENABLE_TTS=OFF` to drop the dependency entirely.

See [NOTES.md](NOTES.md) for implementation details and current limitations,
[the specification](AGENTS/SPEC.md) for planned features, and [LICENSE](LICENSE)
for the GPL-3.0-or-later license.
