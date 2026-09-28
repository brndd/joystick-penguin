# GUI maintainability refactor

## Goal and scope

Make the existing Qt 6 Widgets profile editor understandable and changeable without
rewriting its interface. Keep the typed `joystick_penguin::Config`, YAML format,
validation, and CLI behavior as the sources of truth. This work is a structural
refactor, not the running-remapper integration described in `GUI_SPEC.md`.

Keep Widgets for this refactor. A future QML frontend should be able to call the
same document and editing operations, but do not introduce a QML bridge or rewrite
the current UI/tests solely to prepare for one.

The principal hotspots are `src/gui/editor.cpp` (window construction, history,
mapping forms, action list, validation, navigation and file operations in one
class) and `SetupWorkspace::select()` in `src/gui/setup_workspace.cpp` (all setup
forms, their signals, and usage navigation). Existing `BindingModel`,
`BindingFilter`, `ControlBrowser`, `ProfileHistory`, and `profile_setup` helpers
are starting boundaries to build on, not replacements to duplicate.

## Behavioral contract

- Continue editing in memory as fields change. Save writes YAML; it does not
  apply a profile to a running remapper. Invalid edits remain visible and
  undoable, while invalid saves fail without replacing the original file.
- Preserve the current new/open/save/save-as, dirty marker, undo/redo, setup
  transactions, and consecutive same-mapping edit coalescing. A new mapping,
  change to another mapping, setup edit, save, undo or redo starts a new
  coalescing session as appropriate. Do not silently discard a tap/hold branch
  or incompatible actions when changing input type.
- Keep browsing context where possible: selected controls, mapping/filter/search
  state, action selection, and the mode/modifier usage-table scroll position.
  Resolve source rows through the proxy when navigating from issues or setup.
- Preserve supported input/action variants, literal codes, multiple outputs,
  tap/hold branches and branch moves, labels, offline editing, device setup,
  reference-renaming rules, and actionable profile issues.
- Retain existing visible labels, keyboard shortcuts, accessible names and widget
  `objectName`s used by `tests/gui_editor_test.cpp` unless an intentional UI
  change is separately specified. Keep the 1024x768 and dark/high-DPI review
  fixtures usable.

## Target responsibilities and interfaces

### Profile document (`src/gui/profile_document.hpp/.cpp`)

Own the one mutable `Config`, saved snapshot, path, `ProfileHistory`, dirty
calculation and validation cache. Expose read-only configuration and issues to
views; expose explicit operations to replace/new/open a document, apply an
atomic setup transaction, apply a mapping edit with a coalescing key, edit
profile-wide labels, undo/redo, and mark a successful save. Edits should be
performed through operations accepting a proposed `Config` or a mutation
callback; callers must not mutate the live configuration before the document
has recorded the change. Return/report whether an operation actually changed
the document. Provide change notifications that distinguish full replacement,
binding insertion/removal, and updates where necessary for Qt model signals.

Preserve `Config`'s address across replacement if models/widgets reference it;
otherwise migrate those references in the same step. The document owns validation
results computed when its config changes, using existing `config_issues` and
`validate_edited_config`/save paths, not an independent GUI ruleset. The window
still owns file dialogs and error presentation; the document should not depend
on toolbar, table, or modal widget instances. Make the history policy explicit
in its API and cover coalescing across edits and boundaries with focused tests.

### Window coordinator (`src/gui/editor.hpp/.cpp`)

Own the toolbar, tabs, file-dialog commands, status/issues presentation and
cross-workspace navigation. Wire document notifications to title, validation
and views in one place. Keep only top-level selection/navigation state here;
do not store pointers to individual mapping form fields or action-row widgets.
Retain the deliberate view-before-document destruction order (the current
`EditorWindow` destructor removes views/models before releasing the `Config`
they reference).

### Mapping workspace and binding detail

Extract the mapping tab into `src/gui/mapping_workspace.hpp/.cpp`: control
browser, setup prompt, search/filter/table, add/duplicate/delete actions, and
selected binding coordination. It owns a focused binding-detail widget in
`src/gui/binding_detail.hpp/.cpp` for input identity, modes/modifiers and
tap/hold timing. These widgets display document state and emit edit/navigation
intents; document changes must go through the document operations. Keep
`BindingModel`/`BindingFilter` as the table presentation layer and use their
insert/remove/data-change/reset notifications rather than mutating `Config`
behind an unchanged model. Use an explicit refresh guard or `QSignalBlocker`
when populating forms so rendering cannot create edits.

### Action list and editor

Move the existing inline `ActionEditor` and combined action list into focused
`src/gui/action_editor.hpp/.cpp` and `src/gui/action_list.hpp/.cpp` components.
Represent an action with a typed location: binding row plus branch
`Immediate`/`Tap`/`Hold` and index. Translate table rows to locations in the
action-list component; do not spread integer branch codes and `Qt::UserRole`
conventions across the main window. Centralize add, edit, remove, reorder and
move-between-branches operations, including action compatibility/default choices
for input types. Rendered row buttons must select the intended action even after
a rebuild; discard or remap locations after binding insertion/removal or document
replacement. Keep single-click selection, inline live edits and combined Tap/Hold
list behavior. Constrain widget-specific event filtering (row selection and
label elision) to the action-list component; the window retains only its own
status/issue interaction.

### Setup workspace

Keep `SetupWorkspace` as the list/navigation shell, but extract physical-device,
virtual-device, mode and modifier property construction/updates into focused
widgets or form builders under `src/gui/`. Share the existing `profile_setup`
rename/remove/reference helpers. Setup forms propose a complete transaction to
the document; preserve the current ability to repair an already-invalid mapping
by validating setup fields separately from profile-wide mapping issues. Avoid
callbacks holding stale `original` device values across successful commits;
either refresh the form from the committed document or build the next proposal
from current state. Give transient form widgets clear QObject ownership and
disconnect/delete them on selection change without leaving deferred callbacks
able to edit a different selection. Preserve usage-table navigation and scroll
restoration.

## Delivery sequence

Each step should compile and pass the relevant tests before starting the next;
avoid a single mechanical move of the entire `editor.cpp` constructor.

1. **Baseline and contract tests.** Run the current GUI tests. Add focused
   regression cases only for behavior at risk during extraction: history
   coalescing/boundaries, action branch moves and selection after rebuild, setup
   commits and undo, navigation through filters, and save failure. Keep the
   existing end-to-end tests as the UI contract.
2. **Document owner.** Introduce the document operations and move history,
   saved/path/dirty and cached issues out of `EditorWindow::State`. Route each
   existing edit path through them before moving widgets. Update model signals
   and validate that undo/redo, labels, setup commits, and save state still work.
3. **Actions and detail.** Extract the action list/editor and then the binding
   detail. Replace row/branch conventions at the extraction boundary; preserve
   `objectName`s and edit ordering. Remove the corresponding fields and event
   handling from `EditorWindow::State`.
4. **Mapping tab.** Extract its browser, filters, table and setup prompt. Make
   the window communicate using high-level intents (`showMapping`, `showControl`,
   `requestDeviceSetup`, document edit) instead of reaching into child fields.
5. **Setup forms.** Split one form type at a time, retaining list selection and
   usage-table state in the shell. Check undo/redo and switching selections
   after each form type.
6. **Cleanup.** Remove unused state/helpers, consolidate genuinely shared field
   choices and conversions, update `CMakeLists.txt` sources, and document the
   resulting component relationships. Do not introduce a generic widget
   framework to remove small amounts of duplication.

## Verification and completion

Use the repository's CMake build and CTest after relevant changes. In
particular run `gui_editor`, `profile_setup`, and `control_browser` (the latter
may skip where uinput/evdev access is unavailable). Check semantic round trips
for every shipped example and `joystick-penguin --check` on saved copies; exercise
the existing offscreen GUI test and its optional screenshot fixtures for layout
review. No step is complete if it breaks an existing editing or navigation path
even when unit tests pass.

The refactor is complete when `EditorWindow` primarily coordinates document
commands and workspaces; mapping forms, action editing, and each setup form can
be understood and changed without navigating the whole window constructor;
the document is the sole owner of editable profile state/history/validation;
and the behavioral contract above remains satisfied.
