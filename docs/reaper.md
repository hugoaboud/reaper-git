# REAPER integration

The extension is a REAPER module: it registers one action, adds one menu entry,
polls the host with a timer, and stores its settings in REAPER's resource folder.
Everything it needs from the host comes through REAPER's plug-in API.

## Contents

- [Purpose](#purpose)
- [Entry point and registration](#entry-point-and-registration)
- [API surface](#api-surface)
- [Projects and the repository](#projects-and-the-repository)
  - [Finding the project](#finding-the-project)
  - [Loading a project](#loading-a-project)
  - [Keeping the panel in sync](#keeping-the-panel-in-sync)
- [Guards around the project](#guards-around-the-project)
- [Settings storage](#settings-storage)
- [How the parts relate](#how-the-parts-relate)
- [Diagnostics](#diagnostics)
- [Caveats](#caveats)
- [Changing the integration](#changing-the-integration)

## Purpose

REAPER knows nothing about version control, and this extension does not try to
teach it. Instead it treats the folder of the open project as the unit of work:
if that folder is a git repository, the panel describes it; if it is not, the
panel offers to create one. Nothing is stored inside the project file, so a
project can be shared with someone who does not have the extension.

## Entry point and registration

- `REAPER_PLUGIN_ENTRYPOINT` (in `src/extension.cpp`) is called with a
  `reaper_plugin_info_t` when the module loads, and with `nullptr` when it
  unloads. Loading requests the API table and fails cleanly if a function is
  missing; unloading removes every registration in reverse order.
- One command id, `REAPERGIT_PANEL`, backs a single action "Git: Panel". The
  action has no default shortcut; the accelerator entry exists so the action is
  listed and bindable in REAPER's Actions window.
- `hookcommand` opens the panel when the action runs.
- `hookcustommenu` adds a single **Git** entry to the top of the main File menu.
  The entry opens the panel; pulling and pushing live inside it. The hook is
  handled for both flag values and inserts the entry only when it is missing:
  `flag == 0` (default menu being built) is where Reaper keeps additions the user
  can then customize, but a user whose File menu is already customized is only
  ever called with `flag == 1` (menu about to be shown), and the stored menu does
  not contain earlier default-menu additions. Handling one flag only makes the
  entry disappear on those installations.
- A `timer` callback runs on the main thread and is used to notice project
  changes.
- `ext_name` is registered as `reaper_git` for identification.

## API surface

Only what is needed is requested, in `src/reaper_api.h`:

| Function | Why |
| --- | --- |
| `plugin_register` | Commands, menu hook, timer, action list, unload. |
| `GetMainHwnd` | Parent window for the panel and for the file pickers; also used to hand focus back after a load. |
| `GetResourcePath` | Location of the settings file. |
| `EnumProjects` | Full path of the open project. |
| `GetProjectName` | Fallback when `EnumProjects` gives nothing. |
| `IsProjectDirty` | Whether the project has unsaved changes. |
| `Main_openProject` | Reloading a project after the repository changed its files. |
| `GetProjectPath` | Recording path; requested but not used by the panel today. |

## Projects and the repository

### Finding the project

The path comes from `EnumProjects(-1, …)`. `GetProjectName` is kept only as a
fallback, because it does not reliably return an absolute path; passing a bare
name to the project loader makes REAPER answer "error opening the project".

The panel never asks the user to pick a repository for the Project section: the
folder of the open project *is* the repository. The Repositories list is a
separate, optional bookkeeping of clones made through the panel.

When the folder is not a repository yet, the Project section offers
**Add Git to this Project**. That action does three things in one step:

1. creates the repository in the project folder (`git init`);
2. writes `.gitignore`, adding `Backups/` and `Media/peaks/` while keeping any
   lines the file already had — both folders are recreated on every save and must
   never enter the history;
3. stages everything and creates the first commit, `Project setup`, so the
   project starts from a clean baseline instead of a dirty tree.

If step 2 or 3 fails, the repository is kept and the panel reports what went
wrong; the user can fix the cause and commit from the History section.

### Loading a project

`Main_openProject("noprompt:" + path)` reloads a project after checkout, pull or
stash recovery changed its files. The `noprompt:` prefix skips the save dialog,
which is correct here because the panel refuses to run those actions on a
modified project in the first place.

Two host details matter:

- The panel is modeless, so it survives the load. It refreshes afterwards, and
  reopens itself if REAPER closed it during the load.
- REAPER keeps its "project loaded" splash up until its main window owns the
  foreground again, so after every load the panel hands the foreground back with
  `SetForegroundWindow(GetMainHwnd())`.

### Keeping the panel in sync

A timer callback compares the current project path and the project's dirty flag
with the last values the panel saw. When either changes, the panel refreshes:
opening or saving a project through REAPER is noticed without the user touching
the panel. The same timer is ignored while a load is in progress, so the panel
cannot refresh into a half-loaded project.

## Guards around the project

- **Saved project required.** Every action that writes to disk is refused while
  `IsProjectDirty` is true. The panel explains that the project must be saved
  first; Open, Review and Commit phrase the same rule in their own words.
- **Locked while loading.** The panel disables all actions and ignores commands
  from the moment a load starts until it finishes, because a git action against a
  half-loaded project would act on inconsistent state.
- **Deferred work.** Anything the user starts from a control notification runs
  after that notification returns, never inside it: the host is still processing
  the click, and starting a project load from there leaves both the control and
  the splash stuck.

## Settings storage

- The settings file is `reaper_git.json` in the folder returned by
  `GetResourcePath`, created on first save.
- It holds three things: the list of **servers** (name, URL, username,
  authentication method, secret, optional key passphrase), the list of
  **repositories** (name, folder), and the last selected repository.
- Passwords and passphrases are *obfuscated*, not encrypted: a reversible
  scramble, so the file is not readable at a glance but anyone with the file and
  this source can recover the secret. The Add Server dialog says so.
- A server entry is matched to a repository by URL first, then by host (and port
  when the URL has a scheme), so a second repository on the same server reuses the
  stored credentials.
- Loading the file is best effort: a missing file means "no configuration yet",
  a corrupt file leaves the panel running with empty lists rather than failing.

## How the parts relate

```
REAPER action / File > Git            timer
        │                               │
        ▼                               ▼
   panel::open()                 panel::pollProjectChange()
        │                               │
        └────────── panel state ────────┘
                        │
        ┌───────────────┴───────────────┐
        ▼                               ▼
  settings::Store                  git::Repo
  (reaper_git.json)                (libgit2)
```

The panel is the only place that decides *what* to do; the settings store keeps
what the user configured, and the git service performs the operation. REAPER
itself is only consulted for three facts: which project is open, whether it is
saved, and which folder it lives in.

## Diagnostics

REAPER gives extensions no console, so the extension writes one small log file,
`reaper_git.log`, in the folder returned by `GetResourcePath` (next to
`reaper_git.json`); if that path is not available yet, it goes to the system's
temporary folder. The file is truncated on every load and records:

- the entry point being called and the `caller_version` REAPER passed;
- the result of `REAPERAPI_LoadAPI` and of each registration (command id,
  accelerator, command hook, menu hook, timer);
- every `hookcustommenu` call for the File menu with its menu id and flag, and
  whether the entry was added or already present;
- every request to open the panel through the command.

This makes two different failures easy to tell apart:

- **No log file at all** after starting REAPER means the module was never
  loaded. On Windows that is almost always a missing dependency of the DLL
  (`libstdc++-6.dll` and friends from an unstripped MinGW build); on Linux a
  wrong folder or a broken `libssl`/`libcrypto`. The action also does not appear
  in the Actions window in that case.
- **A log file that stops early** names the registration that failed, and a log
  that records `menu: ... ignored (other menu)` shows the host never offered the
  File menu under the expected identifier.

## Caveats

Each item is marked either **hard limit** (blocked by a dependency, with the
evidence) or **tradeoff** (a choice this project made that can be revisited).

- **Hard limit:** `IsProjectDirty` returns false when REAPER's "prompt to save"
  preference is disabled; the SDK documents it as "always returns 0 if
  'undo/prompt to save' is disabled". The saved-project guard is therefore only
  as strict as that preference.
- **Tradeoff:** the extension must never run during rendering or other
  real-time contexts. Evidence: every entry point (panel actions, menu action,
  timer) runs on the main thread, so this constrains how new entry points are
  added rather than describing a current defect.
- **Tradeoff:** network operations block the main thread. Evidence as in
  [service.md](service.md): the calls are direct, with no worker thread.
- **Tradeoff:** git actions are not part of REAPER's undo history and cannot be
  undone with Ctrl+Z. Evidence: nothing here uses REAPER's undo API; each git
  operation is its own transaction.

## Changing the integration

1. Add a function to `src/reaper_api.h` with a `REAPERAPI_WANT_` line before
   using it; the build fails loudly when the name does not exist in the SDK.
2. Keep registrations symmetric: everything registered on load must be removed
   on unload, in reverse order.
3. Never call the project loader from inside a control notification; let the
   action handler do it.
4. When a change affects the settings format, bump the `version` field and keep
   reading older files.
5. Update this file and [service.md](service.md) whenever a host-level rule
   changes.
