# GUI development milestones

## Goal and approach

Build a Qt interface that lets users create and maintain joystick bindings
visually instead of hand-writing large YAML profiles. Use the existing versioned
YAML format as the profile interchange format and the existing typed `Config`
model and validation as the source of mapping semantics.

Start with a Qt Widgets profile editor alongside the current CLI remapper. The
CLI currently owns the hardware `poll()` loop, signal and terminal handling,
and live reload in `run_hardware()`; converting that loop into a Qt application
is not a prerequisite for useful visual editing. After completing profile setup,
overhaul the editor's navigation and presentation before exposing a shared
runtime controller to the CLI and GUI and adding tray operation. Keep the
engine's event, timer, and output processing serialized.

The milestones below are ordered by dependency. Milestones 1–3 establish full
profile editing and setup; milestone 4 makes those workflows approachable.
Runtime integration and the tray follow.

## 1. Profile editing foundation

- Add serialization for the supported typed `Config` model. Reuse the current
  loader and validation rules rather than introducing GUI-specific YAML
  semantics.
- Establish a validation path for an edited, in-memory profile and return
  actionable errors to the UI. Check the serialized result with the existing
  loader before replacing a file.
- Decide and document the save policy for YAML comments, layout, and anchors:
  serializing the typed model may normalize formatting and expand aliases. Do
  not silently claim to preserve those details. Make saves safe against partial
  writes, and retain the original profile if validation or writing fails.
- Add semantic round-trip tests using the example profiles, including
  `examples/star_citizen.yaml`, which uses anchors and aliases.

**Complete when:** loading, saving, and reloading existing supported profiles
preserves their devices, modes, modifiers, bindings, and actions; an invalid edit
or failed write leaves the original file usable.

## 2. Visual binding editor (complete binding coverage)

- Add a Qt Widgets executable with open, new, save, and save-as profile flows.
- Show a searchable, filterable binding list grouped or filterable by physical
  device, control, mode, and modifier. Provide a detail editor for a selected
  binding; support add, duplicate, delete, and efficient repeated edits.
- Let users select a physical device and button (one-based index or explicit
  EV_KEY code), choose modes and held modifiers, and add one or more virtual
  button actions targeting named output devices.
- Explain binding precedence and show validation errors at the relevant edit,
  including conflicting equally specific bindings and references to missing
  devices, modes, or modifiers. Preserve edits in memory until saved.
- Support physical axes and per-direction hat inputs, virtual axis/hat outputs,
  axis inversion, and cross-device mappings.
- Support persistent mode-selection actions, multiple actions per binding, and
  tap/hold branches with `threshold_ms` and `tap_ms`.
- Make the different behaviors clear in the editor: buttons and hat directions
  capture their selected actions on press; axes reroute live when the mode or
  held modifiers change. Offer only combinations the runtime supports.
- Exercise representative large and mixed profiles as editing fixtures, not
  just simple one-button examples.

**Complete when:** users can create and revise substantial button profiles
without hand-editing YAML; every binding construct currently accepted by the
loader can be opened, edited, and saved through the GUI without losing supported
features; the supplied examples remain valid after a round trip and pass `--check`.

## 3. Visual device and profile setup

- Edit evdev inputs using stable `/dev/input/by-id/` or `by-path/` links and
  the exclusive-grab setting. Edit uinput joystick names, bus/IDs, preset, and
  supported axis ranges and neutral values.
- Create, rename, and remove modes and modifier definitions, with references
  updated or clearly flagged when a name changes.
- Provide read-only device discovery and control identification where feasible;
  distinguish available hardware capabilities from profile-only validation.
  Input monitoring must account for exclusive grabs held by a running remapper.

**Complete when:** a new user can configure devices and a valid mapping profile
from the GUI, with understandable feedback for absent devices or unavailable
controls.

## 4. GUI usability overhaul

- Replace the flat binding-centric workspace with a controller/control browser,
  mappings grouped by physical input, and a compact profile-wide overview for
  large profiles. Show output behavior, mode and modifier conditions, and both
  tap and hold branches without opening action dialogs.
- Add a traditional icon-oriented top toolbar for New, Open, Save, Save As, and
  relevant editing/navigation actions. Keep menus, keyboard shortcuts, tooltips,
  accessible names, and clear unsaved-state feedback.
- Show passive input-activity indicators beside physical controls even before
  input is observed. Monitor readable controllers without an exclusive grab;
  illuminate held buttons and show axis/hat motion or values live. Make the
  browser useful offline and explain monitoring limitations when another process
  grabs a controller. Do not require a separate identify-control dialog.
- Let profiles assign optional human-friendly labels to physical inputs (for
  example, `Trigger` for Button 1). Extend the version 1 typed Config, YAML
  loading, serialization, validation, and round-trip coverage without bumping
  the profile version. Preserve input identity independently of its label.
- Move common mapping and action edits inline; put advanced identifiers, timing,
  output identity, and axis ranges behind deliberate controls. Put explanations
  behind adjacent clickable info buttons rather than permanent prose. Retain
  EV_KEY/EV_ABS literal entry and negative/positive direction terminology.
- Make devices, modes, and modifiers accessible as first-class workspaces;
  provide an approachable first-profile flow, context-preserving navigation,
  actionable validation, and keyboard-accessible editing. Keep all existing
  supported binding constructs and offline editing available.

**Complete when:** a user can find or identify a physical control, label it,
understand all its mappings, and edit/save a substantial profile without needing
to interpret raw event codes for ordinary controls; existing examples still
round-trip semantically and pass `--check`, including profiles without labels.
See the detailed GUI overhaul plan below.

## 5. Running-remapper integration

- Extract start, stop, reload, status, and error reporting from `run_hardware()`
  behind a controller shared by CLI and GUI. Keep event, timer, reload, and
  output-state transitions in the serialized runtime loop; queue GUI requests
  to it rather than accessing engine state directly from the Qt thread.
- Allow an explicit **Apply** action for a saved profile and report whether it
  succeeded. Preserve the current mapping on reload rejection.
- Distinguish a mapping-only reload from changes to virtual device shape,
  identity, or capabilities that require recreating virtual devices and thus
  restarting the remapper. Show the user which operation is needed.
- Report current mode, physical device connection/retry state, virtual-device
  information, and runtime errors without relying on console text parsing.
- Keep the CLI's validation and headless remapping workflows available.

**Complete when:** a binding edited in the GUI can be applied to a running
remapper with an explicit success or useful failure result; a rejected update
does not interrupt its active mappings.

## 6. Tray operation

- Add a system tray icon with profile and mode status and actions to open the
  editor, start/stop remapping, apply/reload, and quit.
- Define window-close and application-exit behavior so stopping the remapper
  releases virtual outputs cleanly. Provide access to the editor and status on
  desktops without an available system tray.
- Decide and document whether starting the GUI starts remapping automatically.

**Complete when:** ordinary profile editing and remapper operation can be done
without a terminal, while CLI use and `--check` continue to work.

## Detailed GUI overhaul plan (milestone 4)

### Information architecture and visual direction

Make the physical control, not the YAML binding row, the primary unit of
navigation: **choose a controller → choose a control → see and edit what it
does**. Keep a compact **All mappings** view for searching and scanning large
profiles. Use three main workspaces: **Mappings**, **Devices**, and **Modes &
modifiers**. A mode selector in Mappings filters the editor view; it does not
change the running remapper's mode.

Use a native-feeling, dense-but-readable Qt Widgets layout that follows the
system light/dark palette. Establish hierarchy with spacing, typography, and
subtle selection backgrounds rather than many bordered group boxes. Let the
control browser and workspace resize; scroll the workspace at smaller window
sizes. Pair color with text or shape for status, and retain visible keyboard
focus. Test high DPI and approximately 1024×768 layouts.

A traditional top toolbar contains recognizable **New**, **Open**, **Save**,
**Save As**, and useful context actions such as **Add mapping** and **Undo/Redo**.
Show the filename and unsaved marker in the window title; do not duplicate the
filename in the toolbar. Disable **Save** when there are no unsaved edits, while
keeping **Save As** available. Do not expose the Qt toolbar-visibility context
menu. Icons have tooltips and accessible names; File/Edit menus and standard
keyboard shortcuts remain available. Keep the toolbar uncluttered by placing
infrequent setup actions in their workspaces.
Distinguish live edits to the in-memory profile from **Save** (write YAML to disk).

### Mappings workspace mockup

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ [New] [Open] [Save] [Save As]  │ [Undo] [Redo]                           │
│ Mappings                 Devices                 Modes & modifiers            │
├──────────────────────┬───────────────────────────────────────────────────────┤
│ Controller [left   ▾] │ Trigger (Button 3)                    [Rename] [⋯]   │
│ [Find control…      ] │ View mode [All modes ▾]            [+ Add mapping]   │
│                      │                                                       │
│ Buttons              │ ┌ SCM Mode ───────────────────────────────────── ⋯ ┐ │
│ ◌ Button 1         2 │ │ Tap  → left-vjoy · Button 3                       │ │
│ ● Trigger          4 │ │ Hold → left-vjoy · Button 63 + Nav Mode          │ │
│ ◌ Button 4         2 │ │ Hold after 500 ms                                 │ │
│ ◌ Button 5  Modifier│ └───────────────────────────────────────────────────┘ │
│ …                    │ ┌ 3 modes · While leftmod is held ─────────────── ⋯ ┐ │
│ Axes                 │ │ Press → left-vjoy · Button 33                    │ │
│ ◌ X            value │ └───────────────────────────────────────────────────┘ │
│ ◌ Y            value │                                                       │
│ Hats                 │ [All mappings: search and compact table]              │
├──────────────────────┴───────────────────────────────────────────────────────┤
│ No configuration issues                                                      │
└──────────────────────────────────────────────────────────────────────────────┘
```

The browser lists controls with configured mappings and, when available,
observed hardware controls. Include configured controls for absent hardware;
offer manual selection for unobserved ones. Show a mapping count or **Unmapped**
beside each control and mark buttons used solely as modifiers. Group mappings
for the selected input by mode and held-modifier conditions. Summarize actual
outputs, not action counts: for example, **Tap → Button 3; Hold → Button 63 +
Nav Mode**. Show output-device names for cross-device actions, inversion for
axes, and all branches even when a tap or hold branch is empty.

**All mappings** presents compact rows such as **Input | Conditions | Output**;
search includes input labels, underlying identifiers, devices, modes,
modifiers, and output actions. Filters have visible labels, not only tooltips.
Selecting an overview row opens its control and mapping without discarding
search/filter context. Add, duplicate, delete, and setup edits preserve browsing
context wherever possible; do not clear filters just to reveal a new row.

### Live control activity and labels

Display a passive indicator for *every* listed physical control. A held button
lights up until release. Axes display current values and visually mark changes;
hats indicate the active component/direction. Use a short visual transition so
brief changes can be noticed, without reporting an axis as perpetually active
due to noise. Treat monitoring as observation, separate from the engine's
event-processing loop. Avoid exclusive grabs; avoid busy polling and throttle
display updates. Handle unplugging, permission errors, and changes to the
selected controller without freezing the editor. Make the monitoring status
visible near the browser if live events cannot be read (including when a
running remapper holds an exclusive grab); still show static capabilities and
allow manual configuration. A no-events timeout should not be reported as a
definite grab failure. No modal identify dialog is required: an observed
control can be selected directly from its live indicator or row.

Allow an optional label for a physical input, scoped to its physical device and
exact control identity (button index versus EV_KEY literal, axis/hat code and
hat direction where applicable). Show **Trigger (Button 1)** or a similar
combination in the editor so identity remains inspectable. Apply the label to
every mapping and search result for that input, including modifier definitions
that reference it. Renaming a label must not rewrite bindings or change runtime
behavior. Define and document version 1 YAML storage for labels, including
uniqueness, invalid references, and what happens when a device or control is
removed or renamed. Older profiles simply load with no labels. Include
load/save/reload tests for labeled controls, unlabeled example profiles, and
both indexed and literal input identities.

### Editing flows and progressive disclosure

Opening a mapping expands it inline, with the selected input already known.
Changing the input is an explicit secondary operation. Show about five entries
at once in searchable **In modes** and **While held** lists, and about three
rows in the overview table by default. The action editor is a nested horizontal
split view: a combined **Name | Type** action list takes about one third of the
width, with the selected action's live editor on the right. In Tap/Hold behavior,
Type shows and can switch each action between Tap and Hold; immediate actions
show Immediate. A single click selects an action to edit; adding selects and
displays it immediately, using the selected action's type (or Hold when none is
selected). Keep **Add action**
below the list and put accessible remove and reorder controls beside each row.
Edits apply to the in-memory profile as they are made and remain undoable.
Reuse a recently chosen output device while keeping the destination clearly
visible. Support cross-device outputs.

For **Tap / hold**, display the threshold and both branches together in the
combined list, even when a branch is empty. Put the less commonly changed tap pulse duration in **Timing
details**. Switching behavior types must explicitly resolve incompatible
actions and be undoable. Only present output/action choices supported for that
input: for example, axes map to axes; button inputs can switch modes. Preserve
all loader-supported mappings, including explicit raw codes and multiple
actions. Present axes with familiar names when known, with **EV_ABS literal**
as an advanced choice, and indexed buttons with **EV_KEY literal** available.
Use **Negative (−)** and **Positive (+)** for component direction; do not
assume a physical axis's negative direction means left or up. A hat-direction
picker may use a spatial diagram for selection, but must also show which
component and sign it stores and must preserve diagonal behavior as separate
component directions.

Put short, word-wrapped explanatory text behind adjacent clickable **ⓘ** buttons
on the relevant section: modes (persistent until switched), modifiers (held
conditions and priority), tap/hold timing, and advanced hardware fields. Avoid
duplicating these with a general conditions help button. Clicking opens a short
popover or inline explanation
that can be dismissed and reached by keyboard. Keep the primary screen free of
paragraph-length help. Labels describe the task (**Controller**, **In modes**,
**While held**, **Switch mode**) while literal code names remain precise.
Unmodified mappings must not be described as requiring that no modifiers are
held: more-specific eligible bindings can override them.

### Setup, onboarding, and feedback

The **Devices** workspace lists controllers and virtual joysticks separately.
Selecting one reveals its properties in place. Prioritize recognizable hardware
name, selected stable link, and read/access status for physical controllers;
put grab and alternate/manual paths in connection details. For virtual devices,
show display name and capabilities first; put bus, vendor/product IDs, and
per-axis minimum/maximum/neutral in an always-visible advanced section. Explain
that virtual versus USB bus changes the advertised device identity. Size the
axis-ranges table to its contents rather than stretching it across the pane.
Provide visually distinct section-heading add buttons and per-item confirmed
removal in both setup lists. Keep availability messages concise.
**Modes & modifiers** groups persistent modes separately from held modifiers;
each mode has a star to select the **default mode** (`initial_mode` in the profile).
Its mapping usage rows navigate on one click to the corresponding mapping, and
returning to the setup tab retains both the sidebar and usage-table scroll positions
for the selected mode or modifier. Renames keep
references updated as in the existing setup model. Default to a 1600×900 window
while remaining usable at approximately 1024×768; do not create an application
settings file solely to persist the window size.

For a new profile, guide the user through choosing or manually entering a
controller, creating a virtual joystick with preset defaults, then selecting
their first control. Create the default mode automatically. If the profile is
empty, show **Map your first control** with a clear manual choice even when
hardware is absent. Existing profiles must open directly without this flow.

Show field errors beside the edit, mapping conflicts with links to both
bindings, and profile-wide issues in a navigable issue panel. Distinguish
profile validity from device availability; an offline controller does not by
itself make a profile invalid. Keep technical loader detail accessible on
demand. The present single exception string and regex extraction of a binding
row can serve an interim implementation, but a complete issue panel needs
structured diagnostic locations or an equivalent reliable issue model.

When milestone 5 adds engine integration, extend the document bar with explicit
running status, current runtime mode, and **Apply**. Distinguish unsaved edits,
saved-but-not-applied changes, and updates requiring a remapper restart. A
rejected Apply must leave the active mapping intact. Do not imply that Save
alone applies a profile to the remapper.

### Delivery and verification

Implement in stages: first static screens for a simple mapping, a large mixed
profile, tap/hold, offline hardware, and a conflict; then shared document/edit
state and stable UI selection; then the control browser, live activity,
overviews, and inline editors; then labels, setup workspaces, onboarding, and
issue navigation; finally polish keyboard access, layout, and themes. Separate
document state, control/activity models, mapping presentation, and device setup
instead of expanding the current monolithic `src/gui/editor.cpp`. Reuse the
typed `Config`, existing loader/saver/validation, setup reference updates, and
read-only discovery logic. Do not change YAML save guarantees or treat observed
hardware capabilities as a substitute for profile validation.

Verify simple and mixed-profile editing, label persistence, offline and
exclusively grabbed hardware feedback, keyboard access, and semantic round
trips for every shipped example. Continue validating saved files with
`--check`; unsuccessful validation or writing must leave the original file
usable.
