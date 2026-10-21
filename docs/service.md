# Git service

`src/git_service.h` and `src/git_service.cpp` wrap libgit2 in a small, explicit
API. The panel never calls libgit2 directly; everything it knows about a
repository comes through this layer.

## Contents

- [Purpose](#purpose)
- [Data types](#data-types)
- [Free functions](#free-functions)
- [The Repo class](#the-repo-class)
  - [Reading state](#reading-state)
  - [Reading history](#reading-history)
  - [Changing the working tree](#changing-the-working-tree)
  - [Talking to the server](#talking-to-the-server)
  - [Stashes](#stashes)
- [Credentials and callbacks](#credentials-and-callbacks)
- [Rules and validations](#rules-and-validations)
- [Errors](#errors)
- [Caveats](#caveats)
- [Changing the service](#changing-the-service)

## Purpose

The panel asks four kinds of questions: what is the state of this repository,
what happened in it, how do I move the working tree, and how do I exchange
commits with a server. The service answers exactly those, in the panel's
vocabulary, and keeps libgit2 details (objects, references, callbacks) out of the
interface code.

One deliberate choice: all git pointers are owned inside the `Repo` object, which
is move-only and frees everything in its destructor. Callers work with plain
strings, vectors and a `std::string *error` out-parameter.

## Data types

| Type | Contents |
| --- | --- |
| `Credentials` | Optional username, password, SSH key path and key passphrase. |
| `CommitInfo` | Abbreviated and full id, summary, author, timestamp, parent count, whether the commit is in HEAD's history, whether it is a stash entry. |
| `StashInfo` | Stash index, id, summary, originating branch, author, timestamp. |
| `AheadBehind` | Counts for the local side and the server side, plus whether an upstream exists. |
| `PullOutcome` | `Failed`, `UpToDate`, `FastForwarded`, `Merged`. |

## Free functions

| Function | Behaviour |
| --- | --- |
| `init`, `shutdown` | Start and stop the libgit2 library. Called once by the extension entry point. |
| `isRepository(path)` | True when the folder is inside a repository. |
| `initRepository(path, error)` | Creates a repository in the folder ("Add Git to this Project"). |
| `clone(url, destPath, creds, error)` | Clones into a new folder with the given credentials. |
| `checkAuthentication(url, creds, error)` | Validates credentials against a *server* URL, without a repository. For SSH the full handshake happens; for HTTP(S) only an explicit rejection (401/403) counts as failure, because a bare host cannot authenticate. |
| `checkRemote(url, creds, error)` | Older, repository-less remote check; kept for diagnostics. |

## The Repo class

`Repo::open(path, error)` is the only way in. Every other method assumes an open
repository and reports "no repository" otherwise.

### Reading state

- `workdir`, `remoteUrl` — where the working tree is and which URL the default
  remote points at.
- `currentBranch`, `isHeadDetached`, `isHeadUnborn` — the branch name, whether
  HEAD is detached (reviewing a commit), and whether the branch has no commits
  yet. An unborn HEAD reports its branch name instead of failing.
- `hasChanges` — tracked or untracked modifications exist.
- `aheadBehind(branch)` — local and server counts, and whether an upstream is
  configured.
- `lastFetchTime` — modification time of `FETCH_HEAD`.
- `lastCommit` — the commit HEAD points at, or "no commits yet".

### Reading history

- `log(maxCount)` — the commits reachable from HEAD.
- `logAll(maxCount)` — the whole visible history: every ref plus HEAD. Each entry
  says whether it is in HEAD's history, and stash entries are marked and shown
  as "Stash: …". The helper commits a stash creates (its index and untracked
  trees) are filtered out.
- `branches`, `remoteBranches` — local names, and names such as `origin/master`.
- `incomingCommits(branch, maxCount)` — commits the server branch has and the
  local branch does not, i.e. the "server" side of ahead/behind.

### Changing the working tree

- `checkoutBranch(name)` — switches to an existing local branch.
- `checkoutRemoteBranch("origin/x")` — reuses local `x` if present, otherwise
  creates it at the remote branch, makes it track that branch, and checks it out.
- `checkoutCommit(oid)` — detaches HEAD at a commit; this is "review".
- `commitAll(message)` — stages everything (including deletions) and commits.
  Refuses to create an empty commit.
- `undoLastCommit()` — moves the branch back one commit; used when a push failed
  after the panel created a commit.
- `discardChanges()` — restores tracked files to HEAD.
- `discardAllChanges()` — restores tracked files and deletes untracked files and
  the folders that only held them, so the tree equals HEAD.

### Talking to the server

- `fetch(creds)` — downloads and updates the remote-tracking branches. Before
  fetching it repairs a missing fetch refspec, and afterwards it records the
  upstream for the current branch when the server has that branch. Without both,
  git reports "the upstream is gone" and pull/push have nothing to compare
  against.
- `pull(creds)` — fetches, then compares HEAD with the upstream (or
  `FETCH_HEAD`): up to date, fast-forward, or a merge commit. It never leaves a
  conflicting state behind silently; a failed merge is reported as such.
- `push(branch, creds, error)` — pushes `branch` to the same name on the server,
  sets the upstream when it was missing, and then asks the server for its
  references to confirm that branch now points at the pushed commit. A mismatch
  is an error even though the transfer itself succeeded.
- `mergeBranch(name)` — creates a merge commit of another branch; used by the
  pull path.

### Stashes

- `stashSave(message)` — stashes tracked *and* untracked changes with an explicit
  author signature.
- `stashes()` — newest first, with the originating branch.
- `stashPop(index)` — restores a stash without merging: it computes which paths
  the stash changed, checks those files out of the stash's tree (the stashed
  version wins), and then drops the entry. Any failure restores the working tree
  to HEAD, so a failed recovery leaves no half-merged project. It never produces
  conflict markers, which matters because a REAPER project mixes text and binary
  data.

## Credentials and callbacks

- `Credentials` is filled by the panel from the matching stored server (see
  [reaper.md](reaper.md)); the service only consumes it.
- The credential callback answers with the method the user configured, not the
  first method the server offers. An SSH server commonly advertises both public
  key and password; answering a password configuration with a key used to abort
  the connection and produce a misleading error.
- One payload structure is shared by the callbacks of a remote operation;
  push-status tracking and credentials travel together.
- Push rejections reported by the server are turned into readable errors.
- SSH host keys are verified twice: libgit2 checks the machine's `known_hosts`
  first, and a key that is not there is checked against the fingerprints stored
  in the settings. An unknown key asks the user once (`git::setHostKeyPrompt`,
  wired to a confirmation dialog by the panel) and is remembered afterwards; a
  key that differs from the stored one is always refused, because it may mean
  the host was replaced. Windows has no OpenSSH configuration, so this store is
  what makes SSH usable there.

## Rules and validations

The service enforces what it can own, and the panel enforces workflow rules:

- Commit is impossible on a detached HEAD: the panel blocks it and the
  commit-or-stash helper refuses to commit while reviewing.
- Recover refuses to run while the working tree is dirty or HEAD is detached;
  the panel states the same rule before calling.
- Pull and push only make sense with an upstream: pull reports "nothing to pull"
  when there is no upstream or nothing is behind, push reports "nothing to push"
  when there is nothing ahead.
- A failure inside stash recovery or a failed push never leaves the repository in
  a partially changed state.
- Paths and names coming from the user are normalised (slashes, trailing
  separators, stray whitespace) before they are stored or compared.

## Errors

- Every fallible method takes `std::string *error`; it is only written on
  failure and contains a short action ("could not open the remote") plus
  libgit2's message when that message is meaningful.
- libgit2's stale default-configuration errors ("ProgramData", "doesn't exist",
  "could not find") are dropped, because they describe a probe the user cannot
  act on.
- The panel turns these strings into a message box and a status line; it never
  shows a raw libgit2 code.

## Caveats

Each item is marked either **hard limit** (blocked by a dependency, with the
evidence) or **tradeoff** (a choice this project made that can be revisited).

- **Tradeoff:** everything is synchronous on the calling thread. Evidence: the
  panel calls `git::Repo` methods directly and the service never spawns a
  thread. Large fetches or pushes therefore block the panel; moving them to a
  worker would need progress reporting and careful reuse of the panel state.
- **Tradeoff:** merge conflicts are not resolvable in the panel. Evidence: the
  service reports a failed merge and stops; there is no conflict listing or
  editor. Adding one is a feature, not a library limitation.
- **Tradeoff:** `logAll` caps the number of commits it returns; the panel asks
  for a few hundred. Raising the cap only costs time.
- **Tradeoff:** fetching verifies and repairs tracking configuration as a side
  effect. Evidence: `fetch` adds a missing refspec and records the upstream when
  the server has the branch. It is deliberate, but it means fetch is not purely
  read-only.

## Changing the service

1. Keep libgit2 types out of the header; the interface works with plain values.
2. Every new method takes `error`, cleans up all libgit2 objects on every path,
   and leaves the repository unchanged when it fails.
3. New server operations must use the shared callback setup, so credentials and
   push status behave the same everywhere.
4. When a rule belongs to the workflow (for example "no commit while
   reviewing"), enforce it in the service as well as the panel, so a second
   caller cannot bypass it.
5. Update this file and [ui.md](ui.md) when a rule or a user-visible state
   changes.
