# User interface

The interface is one modeless panel plus a handful of modal dialogs, all built
from the same small set of helpers in `src/ui.cpp` and `src/panel.cpp`.

## Contents

- [Purpose](#purpose)
- [Where the code lives](#where-the-code-lives)
- [How windows are built](#how-windows-are-built)
  - [Windows and dialogs](#windows-and-dialogs)
  - [Controls](#controls)
  - [Positioning](#positioning)
- [Visual rules](#visual-rules)
- [Interaction rules](#interaction-rules)
- [Hint boxes](#hint-boxes)
- [Lists](#lists)
- [Caveats](#caveats)
- [Changing the interface](#changing-the-interface)

## Purpose

The panel describes the repository that contains the open project and offers
the few actions that make sense while working on a recording: exchange commits
with a server, review an old commit, and keep stashes. It is deliberately not a
general git client, so it stays flat: a column of sections, no menus, no tabs.

The sections are, in order: **Servers** (stored credentials per server),
**Repositories** (local clones known to the extension), **Project** (the open
project's repository), **History** (commits) and **Stash**.

## Where the code lives

| File | Responsibility |
| --- | --- |
| `src/ui.h`, `src/ui.cpp` | Cross-platform windows, controls, list and combo helpers, the native pickers, the dark dialog theme, and the reusable dialogs (alert, confirm, prompt, checkbox, commit-or-stash). |
| `src/panel.cpp` | The panel: palette, state, refreshers, layout, hint boxes, panel-specific dialogs, action handlers, window procedure. |
| `src/panel.h` | Only what other files need: open, close, refresh, status line, project polling. |

`panel.cpp` reads top to bottom as: identifiers and palette, `Panel` state, data
refreshers, state and layout passes, dialogs, actions, window procedure, public
API. New code belongs in the matching group.

## How windows are built

### Windows and dialogs

- A dialog is a *build function* that creates its controls plus a window
  procedure. `ui::showModal` runs it modally, `ui::createWindow` creates the
  modeless panel.
- Controls are created once, when the window exists (`ui::isInitMessage`).
  Nothing recreates controls later; only their text, visibility and position
  change.
- On Linux the dialog is described by a `SWELL_DialogResourceIndex` with a build
  function, which is SWELL's supported path: it owns painting, Enter/Escape/Tab
  handling and sizing. On Windows an in-memory template is used and the controls
  are created directly.
- Every dialog procedure starts with `ui::themeMessage`, which paints the dark
  background, the text colour and the owner-drawn buttons.

### Controls

- `ui::addControl` wraps a Win32 class name (`"BUTTON"`, `"EDIT"`, `"STATIC"`,
  `"COMBOBOX"`, `"SysListView32"`), so one call works on both platforms.
- `ui::addButton` is the standard push button: owner-drawn, so it matches the
  panel palette.
- Combo boxes use `ui::comboHeight()`, which is the closed height on Linux and
  the dropped-down height on Windows.

### Positioning

- `layout(Panel&)` is the only place that assigns geometry. It walks the
  sections from the top, moves every visible control and ends with a repaint.
- It runs only at defined moments: window creation, `WM_SIZE`, a section toggle,
  an explicit refresh, the project-load lock, and when the number of visible
  hint lines changes. Nothing else moves controls.
- A collapsed section must hide its controls, not merely skip positioning them;
  leftover controls from an earlier layout are the classic bug here.

## Visual rules

- The panel background is dark grey; text on it is light grey.
- Section headers are the only coloured controls: dim grey when collapsed, dim
  bluish teal when expanded. Their text never changes; the colour carries the
  state.
- Action buttons have a light face with black text; disabled buttons use a dark
  face with muted text.
- Hint panels are flat rectangles in the collapsed-section grey, with light grey
  text that paints no background of its own.
- The "project not saved" message is a light yellow panel with black text.
- The bottom row holds the status message on the left and the plug-in version on
  the right (`v<version>`, right-aligned). The version is compiled in from the
  `REAPER_GIT_VERSION` CMake variable, which `scripts/build.sh` asks for, so it
  changes per build without touching the sources.
- Warning labels (for example the irreversible "delete the folder" toggle in the
  remove-repository dialog) use a system checkbox with an empty caption plus a
  separate static label painted in light yellow. Evidence: SWELL paints a
  checkbox caption with its own theme colours and ignores the dialog's colour,
  so the text has to live in a control whose colour the dialog controls.
- History rows carry meaning by text colour: blue for the current commit, grey
  for commits outside the current history, yellow for the uncommitted-changes
  row and for stash entries.

## Interaction rules

- Every action that writes to disk requires a saved project. One guard in the
  panel command handler refuses the action and explains why; Open, Review and
  Commit phrase their own warning.
- While a project loads, the panel is locked: all buttons, section headers and
  the branch selector are disabled, the status line says so, and commands are
  ignored until the load finishes.
- Disabled buttons explain themselves through the section's hint box. "Select a
  row first" is never explained, because an empty selection makes it obvious.
- Irreversible actions ask first and spell out the consequence in the question.
- Operations run on the calling thread. The status line says what is happening
  before and after, so a slow network operation is at least visible.

## Hint boxes

- Each section owns at most one hint box, listing one line per disabled action:
  a bold title ("Pull:") and the reason.
- The box reserves space only for the lines it shows, and disappears when its
  section is collapsed or nothing needs explaining.
- Hints are produced in `updateActionStates`, the same function that enables and
  disables buttons, so a button's rule and its sentence cannot drift apart.

## Lists

- `ui::listAddRow`, `ui::listClear`, `ui::listSelection` and `ui::listSelect`
  wrap the list view. Lists repaint themselves after a change; the rest of the
  panel needs `ui::refreshWindow`.
- History can start with a synthetic row for uncommitted changes. Any code that
  maps a selected row to a commit must subtract `Panel::historyRowOffset`.
- Long text is truncated with ASCII dots, because SWELL's fonts may lack
  typographic characters.

## Caveats

Each item is marked either **hard limit** (blocked by a dependency, with the
evidence) or **tradeoff** (a choice this project made that can be revisited).

- **Hard limit:** SWELL's list view cannot paint row backgrounds. Evidence: its
  painting code sends `NMLVCUSTOMDRAW` with `clrText` per cell and never reads
  `clrTextBk`. Row emphasis is therefore text colour; painted rows would mean
  drawing the lists ourselves.
- **Hard limit:** SWELL's combo popup is a native menu and sends its change
  notification from the mouse handler that opened it. Evidence:
  `swell-wnd-generic.cpp` builds a popup menu in the combo's button-up case and
  sends `CBN_SELCHANGE` before returning, so anything started there runs while
  the menu is still unwinding.
- **Tradeoff:** `MessageBox`-based alerts and confirmations keep the system look.
  Evidence: `ui::alert` and `ui::confirm` call `MessageBox` directly. Replacing
  them with themed dialogs is possible but gives up native modality and default
  button behaviour.
- **Tradeoff:** the layout grows downwards and does not scroll. Evidence:
  `layout` starts at a fixed top margin and adds heights. A scrollable content
  area is a feature to add, not a platform limitation.

## Changing the interface

1. Create the control in `buildPanel` (or the dialog's build function), position
   it in `layout`, and add it to the section's visibility list.
2. Decide when it is enabled in `updateActionStates`, and give the disabled state
   a hint line unless the reason is obvious.
3. Use `ui::addButton` for push buttons and the palette constants for any custom
   drawing; never hard-code colours elsewhere.
4. Never run long work from a control notification; hand it to the action
   handler that runs after the notification returns.
5. Check the section with both a project inside a repository and one outside it,
   because the Project section has distinct states.
