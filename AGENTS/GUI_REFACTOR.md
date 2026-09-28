# GUI refactor progress

Implementation plan: [AGENTS/GUI_REFACTOR_SPEC.md](GUI_REFACTOR_SPEC.md).

## Current status

| Delivery step | Status | Progress |
| --- | --- | --- |
| 1. Baseline and contract tests | Complete | Added focused history tests and GUI checks for setup undo/redo, navigation through a hiding search filter, and editing an action after a Tap/Hold branch move. |
| 2. Document owner | Complete | `ProfileDocument` owns the stable `Config`, history, saved snapshot/path, dirty calculation and cached issues. Window edits now propose copies through document operations; change notifications drive binding model updates. Added document history/notification coverage. |
| 3. Actions and detail | Complete | `ActionEditor` owns the inline form; `ActionList` owns combined action rows, typed binding/branch/index locations, selection, branch moves, inline editing and row event handling; `BindingDetail` owns input identifiers, modes/modifiers, tap/hold timing, summary and issue display. The window coordinates the selected binding and browsing filters through callbacks without storing individual form fields. |
| 4. Mapping tab | Complete | `MappingWorkspace` owns the control browser, search/filter/table, binding detail, add/duplicate/delete commands and device setup prompt. The window uses source-row navigation and high-level setup requests. |
| 5. Setup forms | Complete | Physical/virtual device and mode/modifier property widgets own their fields and edit callbacks. `SetupWorkspace` retains list selection, field-only transaction validation, usage navigation and scroll state. Proposals read the current config so successive edits cannot restore stale device properties; removed forms disconnect before deferred deletion. |
| 6. Cleanup | Complete | Separated setup form disposal and usage-table construction, clarified widget constructor sections and ownership, removed an unused include, and documented component relationships. Existing CMake GUI source list includes all extracted components. |

The initial action editor and regression coverage landed in `1dae326` (`Start GUI refactor with action editor and regression coverage`); the document owner landed in `2abb429` (`Extract GUI profile document owner`), and the mapping workspace in `665dae7` (`Extract GUI mapping workspace`). The action-list, binding-detail, mapping-workspace and setup-form extractions build and pass the full CTest suite (12/12 tests), including offscreen GUI, profile setup and control browser tests. GUI regressions include successive physical-device edits and modifier renaming as well as canceling an incompatible input change with populated Tap/Hold branches.

## Verification

The cleanup builds with the repository's CMake configuration. Full CTest passes
(12/12), including `gui_editor` offscreen, `profile_setup`, and `control_browser`.
`config_save` checks semantic and disk round trips for all six shipped examples,
and `joystick-penguin --check` accepts each example. Existing usage scroll state
and setup form lifetime handling remain in `SetupWorkspace`; field-specific
choices stay with their respective forms rather than adding a generic widget layer.
The dark, high-DPI offscreen GUI fixture also passes and produces its screenshot.
