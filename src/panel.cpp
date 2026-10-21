#include "panel.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

#include "git_service.h"
#include "reaper_api.h"
#include "settings.h"
#include "ui.h"
#include "util.h"

// The build sets this from CMake (-DREAPER_GIT_VERSION=...); the fallback keeps
// a plain build working.
#ifndef REAPER_GIT_VERSION
#  define REAPER_GIT_VERSION "dev"
#endif

// SWELL does not know every list-view style flag; it is a no-op there.
#ifndef LVS_SHOWSELALWAYS
#  define LVS_SHOWSELALWAYS 0
#endif

namespace panel
{

namespace
{

enum ControlId
{
  IDC_SEC_REMOTES = 1000,
  IDC_SEC_REPOS,
  IDC_SEC_PROJECT,
  IDC_SEC_HISTORY,
  IDC_SEC_STASH,

  IDC_REMOTE_LIST = 1010,
  IDC_REMOTE_ADD,
  IDC_REMOTE_REMOVE,

  IDC_REPO_LIST = 1020,
  IDC_REPO_CLONE,
  IDC_REPO_ADD,
  IDC_REPO_REMOVE,
  IDC_REPO_OPEN,

  IDC_BRANCH_COMBO = 1030,
  IDC_FETCH,
  IDC_STASH_LIST,
  IDC_RECOVER,
  IDC_PULL,
  IDC_PUSH,
  IDC_ADD_GIT,

  IDC_HISTORY_LIST = 1040,
  IDC_PREVIEW,
  IDC_COMMIT,
  IDC_CLEAR_CHANGES,

  IDC_INCOMING_LIST = 1060,

  IDC_STATUS = 1050,
};

// What the Project section can say about the open project.
enum class ProjectState
{
  NoProject, // never saved
  NotARepository,
  Ready
};

// Panel chrome colours (the panel paints its own background, section headers,
// action buttons and hint boxes; SWELL does not expose per-window colours).
const COLORREF kPanelBackground = RGB(38, 38, 38);
const COLORREF kSectionClosed = RGB(74, 74, 74);
const COLORREF kSectionOpen = RGB(84, 146, 148); // dim bluish teal
const COLORREF kTextOnDark = RGB(200, 200, 200);
const COLORREF kTextOnTeal = RGB(235, 245, 245);
const COLORREF kButtonFace = RGB(200, 200, 200);
const COLORREF kButtonFaceDisabled = RGB(80, 80, 80);
const COLORREF kButtonText = RGB(0, 0, 0);
const COLORREF kButtonTextDisabled = RGB(140, 140, 140);
// Message panel ("Project not saved to disk."): light yellow, black text.
const COLORREF kMessageBackground = RGB(255, 248, 190);

// One line of an action-hint box: a bold button title and its reason.
struct HintLine
{
  HWND title = nullptr;
  HWND reason = nullptr;
};

// Hint boxes show up to kHintLines reasons and shrink to the lines in use.
constexpr int kHintLines = 3;

// A bordered box, one line per disabled button. This is how the panel documents
// why each action is unavailable.
struct HintBox
{
  HWND box = nullptr;
  HintLine lines[kHintLines];
  int lineCount = 0;
  // Whether the owning section is expanded; collapsed sections hide their box.
  bool sectionOpen = true;
};

HFONT g_hintFont = nullptr;
HFONT g_hintBoldFont = nullptr;
HFONT g_boldFont = nullptr;

void createHintBox(HWND parent, HintBox &hint)
{
  hint.box = ui::addControl(parent, "BUTTON", "", 0, BS_OWNERDRAW, -100, -100, 10, 10);
  for (auto &line : hint.lines)
  {
    line.title = ui::addControl(parent, "STATIC", "", 0, SS_LEFT, -100, -100, 10, 10);
    line.reason = ui::addControl(parent, "STATIC", "", 0, SS_LEFT, -100, -100, 10, 10);
    ui::setFont(line.title, g_hintBoldFont);
    ui::setFont(line.reason, g_hintFont);
  }
}

// Height needed for the lines currently shown (no reserved empty space).
int hintBoxHeight(const HintBox &hint)
{
  if (!hint.sectionOpen || hint.lineCount <= 0) return 0;
  // A Windows font is taller than SWELL's, so the row height follows the font
  // instead of assuming 15 pixels.
  const int line = std::max(15, ui::lineHeight(g_hintFont));
  return 6 + hint.lineCount * line;
}

void setHintLines(HintBox &hint, const std::vector<std::pair<std::string, std::string>> &entries)
{
  hint.lineCount = static_cast<int>(entries.size());
  if (hint.lineCount > kHintLines) hint.lineCount = kHintLines;

  for (int i = 0; i < kHintLines; ++i)
  {
    const bool show = hint.sectionOpen && i < hint.lineCount;
    ui::setText(hint.lines[i].title, show ? entries[static_cast<std::size_t>(i)].first : "");
    ui::setText(hint.lines[i].reason, show ? entries[static_cast<std::size_t>(i)].second : "");
    ui::setVisible(hint.lines[i].title, show);
    ui::setVisible(hint.lines[i].reason, show);
  }
  ui::setVisible(hint.box, hint.sectionOpen && hint.lineCount > 0);
}

void layoutHintBox(HintBox &hint, int x, int y, int width)
{
  hint.sectionOpen = true;
  const int height = hintBoxHeight(hint);
  ui::setVisible(hint.box, height > 0);
  if (height <= 0) return;

  ui::moveControl(hint.box, x, y, width, height);
  const int line = std::max(15, ui::lineHeight(g_hintFont));
  for (int i = 0; i < kHintLines; ++i)
  {
    const bool show = i < hint.lineCount;
    ui::setVisible(hint.lines[i].title, show);
    ui::setVisible(hint.lines[i].reason, show);
    if (!show) continue;
    const int lineY = y + 2 + i * line;
    ui::moveControl(hint.lines[i].title, x + 6, lineY, 74, line - 1);
    ui::moveControl(hint.lines[i].reason, x + 82, lineY, width - 88, line - 1);
  }
}

void hideHintBox(HintBox &hint)
{
  hint.sectionOpen = false;
  ui::setVisible(hint.box, false);
  for (auto &line : hint.lines)
  {
    ui::setVisible(line.title, false);
    ui::setVisible(line.reason, false);
  }
}

struct Panel
{
  HWND hwnd = nullptr;

  // "Servers" section (stored as remotes).
  HWND remotesHeader = nullptr;
  HWND remoteList = nullptr;
  HWND addRemote = nullptr;
  HWND removeRemote = nullptr;
  HintBox remotesHint;

  HWND reposHeader = nullptr;
  HWND repoList = nullptr;
  HWND cloneRepo = nullptr;
  HWND addLocalRepo = nullptr;
  HWND removeRepo = nullptr;
  HWND openProjectButton = nullptr;
  HintBox reposHint;

  HWND projectHeader = nullptr;
  HWND repoLabel = nullptr;
  HWND projectMessage = nullptr;
  HWND addGitButton = nullptr;
  HWND branchLabel = nullptr;
  HWND branchCombo = nullptr;
  HWND trackLabel = nullptr;
  HWND fetchTimeLabel = nullptr;
  HWND fetchButton = nullptr;
  HWND incomingLabel = nullptr;
  HWND incomingList = nullptr;
  HWND pullButton = nullptr;
  HWND pushButton = nullptr;
  HintBox projectHint;

  HWND historyHeader = nullptr;
  HWND historyList = nullptr;
  HWND previewButton = nullptr;
  HWND commitButton = nullptr;
  HWND clearChangesButton = nullptr;
  HintBox historyHint;

  HWND stashHeader = nullptr;
  HWND stashLabel = nullptr;
  HWND stashList = nullptr;
  HWND recoverButton = nullptr;
  HintBox stashHint;

  HWND statusLabel = nullptr;
  HWND versionLabel = nullptr;

  // Branch combo contents, in combo order (see refreshProject).
  std::vector<std::string> branchItems;

  // 1 while the "[UNCOMMITED CHANGES]" row occupies the first history row.
  int historyRowOffset = 0;

  // Text colour per history row: 0 normal, 1 grey (not on HEAD), 2 blue
  // (current commit), 3 yellow (uncommitted changes).
  std::vector<int> historyRowColors;

  // Branch combo entry shown while HEAD is detached (the commit summary).
  std::string previewItem;

  // Name of the branch HEAD is on (empty when detached/unborn).
  std::string currentBranchName;

  bool remotesOpen = false;
  bool reposOpen = false;
  bool projectOpen = true;
  bool historyOpen = true;
  bool stashOpen = false;

  // What the Project section currently shows (message/add-git/normal).
  ProjectState projectState = ProjectState::NoProject;
  bool hasUncommittedChanges = false;
  bool hasIncoming = false;

  // True while a project is loading: every action is locked.
  bool busy = false;

  // Total hint lines currently shown; a change requires a re-layout because
  // the hint boxes shrink/grow with their content.
  int hintLinesTotal = 0;

  std::vector<git::StashInfo> stashes;
  std::vector<git::CommitInfo> commits;
};

Panel *g_panel = nullptr;

settings::Store &store() { return settings::Store::get(); }

void setStatus(Panel &p, const std::string &message)
{
  if (p.statusLabel) ui::setText(p.statusLabel, message);
}

std::string repoBaseDirectory()
{
  std::string resource;
  if (GetResourcePath) resource = GetResourcePath();
  if (resource.empty()) return "reaper_git_repos";
  return util::pathJoin(resource, "GitRepositories");
}

std::string deriveRepoName(const std::string &url)
{
  std::string path = url;
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  std::string name = util::fileName(path);
  if (util::endsWith(util::toLower(name), ".git")) name = name.substr(0, name.size() - 4);
  return util::sanitizeName(name);
}

// ssh://host/path, or the scp-like user@host:path form.
bool isSshUrl(const std::string &url)
{
  const std::string lower = util::toLower(url);
  if (util::startsWith(lower, "ssh://")) return true;
  return lower.find("://") == std::string::npos && lower.find('@') != std::string::npos &&
         lower.find(':') != std::string::npos;
}

// Path of the project file currently open in REAPER ("" when unset/unsaved).
std::string currentProjectFile()
{
  char buffer[4096] = {0};
  if (EnumProjects) EnumProjects(-1, buffer, static_cast<int>(sizeof(buffer)));

  std::string path = util::normalizeSlashes(buffer);
  if (path.empty() && GetProjectName)
  {
    char nameBuffer[4096] = {0};
    GetProjectName(nullptr, nameBuffer, static_cast<int>(sizeof(nameBuffer)));
    path = util::normalizeSlashes(nameBuffer);
  }

  if (!util::isAbsolutePath(path)) return {};
  return path;
}

// Last project state the panel refreshed for; used to notice project loads and
// saves (the latter changes IsProjectDirty).
std::string g_lastProjectFile;
int g_lastProjectDirty = -1;

void refreshProjectIfChanged()
{
  if (!g_panel || !g_panel->hwnd) return;
  // Do not refresh (or re-enable anything) while a load is in progress.
  if (g_panel->busy) return;
  const std::string file = currentProjectFile();
  const int dirty = (IsProjectDirty && IsProjectDirty(nullptr) != 0) ? 1 : 0;
  if (file == g_lastProjectFile && dirty == g_lastProjectDirty) return;
  g_lastProjectFile = file;
  g_lastProjectDirty = dirty;
  refresh();
}

// The Project/History sections work on the repository that contains the open
// project, never on the selection in the Repositories list.
std::string effectiveRepoPath(Panel & /*p*/)
{
  const std::string file = currentProjectFile();
  if (file.empty()) return {};
  const std::string dir = util::dirName(file);
  if (dir.empty()) return {};
  return git::isRepository(dir) ? dir : std::string();
}

git::Credentials credentialsFor(git::Repo &repo)
{
  git::Credentials creds;
  std::string error;
  const std::string url = repo.remoteUrl(&error);
  const settings::Remote *remote = url.empty() ? nullptr : store().findRemoteForUrl(url);
  if (!remote) return creds;

  creds.username = remote->username;
  if (remote->auth == settings::AuthMethod::SshKey)
  {
    creds.sshKeyPath = util::expandHome(remote->secret);
    creds.passphrase = remote->passphrase;
  }
  else
  {
    creds.password = remote->secret;
  }
  return creds;
}

void updateActionStates(Panel &p);
void layout(Panel &p);

// Locks every action in the panel while a project is loading, so the UI cannot
// start a git operation against a half-loaded project.
void setBusy(Panel &p, bool busy);

// Warns (and returns true) when the open project has unsaved changes.
bool warnIfProjectDirty(Panel &p, const char *action);

void reloadCurrentProject(Panel &p)
{
  // Use the same project-path source as the Project section (EnumProjects):
  // GetProjectName does not reliably give a full path, and passing a bare name
  // to Main_openProject makes REAPER report "error opening the project".
  const std::string file = currentProjectFile();
  if (file.empty()) return;

  const std::string argument = "noprompt:" + file;
  setBusy(p, true);

#ifdef _WIN32
  // REAPER's splash keeps covering and uncovering the panel while the project
  // loads, and every uncover repaints the list views - that is the flicker. Stop
  // them painting for the duration and refresh them once afterwards.
  const HWND lists[] = {p.remoteList, p.repoList, p.incomingList, p.historyList, p.stashList};
  for (HWND list : lists)
    if (list) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
#endif

  Main_openProject(argument.c_str());

#ifdef _WIN32
  for (HWND list : lists)
    if (list)
    {
      SendMessageW(list, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(list, nullptr, TRUE);
    }
#endif

  setBusy(p, false);

  // REAPER keeps its "project loaded" splash up until the main window owns the
  // foreground again; the panel took focus for the load, so hand it back.
  if (GetMainHwnd)
  {
    HWND mainWindow = GetMainHwnd();
    if (mainWindow) SetForegroundWindow(mainWindow);
  }

  // One full repaint, children included: the load cleared and refilled the lists
  // while the panel was disabled, and those controls need a fresh paint.
  if (p.hwnd) ui::refreshWindowDeep(p.hwnd);
}

// Call sites operate on the single global panel.
void reloadCurrentProject()
{
  if (g_panel) reloadCurrentProject(*g_panel);
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void refreshRemotes(Panel &p)
{
  ui::listClear(p.remoteList);
  for (const auto &remote : store().remotes())
  {
    ui::listAddRow(p.remoteList, {remote.name, remote.url, remote.username,
                                  remote.auth == settings::AuthMethod::SshKey ? "SSH key"
                                                                              : "Password"});
  }
}

void refreshRepositories(Panel &p)
{
  ui::listClear(p.repoList);
  const auto &repos = store().repositories();
  for (const auto &repo : repos)
  {
    // Which stored remote (if any) the clone authenticates with.
    std::string remoteName = "-";
    git::Repo handle;
    std::string error;
    if (handle.open(repo.path, &error))
    {
      const std::string url = handle.remoteUrl(&error);
      if (!url.empty())
      {
        if (const settings::Remote *remote = store().findRemoteForUrl(url))
          remoteName = remote->name;
      }
    }
    ui::listAddRow(p.repoList, {remoteName, repo.name, repo.path});
  }
}

void refreshProject(Panel &p)
{
  p.stashes.clear();
  p.branchItems.clear();
  p.previewItem.clear();
  p.currentBranchName.clear();
  p.hasIncoming = false;
  p.hasUncommittedChanges = false;
  ui::listClear(p.stashList);
  ui::listClear(p.incomingList);
  ui::comboClear(p.branchCombo);
  ui::comboSelect(p.branchCombo, -1);
  ui::setText(p.branchLabel, "Branch:");

  const std::string projectFile = currentProjectFile();
  const std::string path = effectiveRepoPath(p);

  if (projectFile.empty())
  {
    // Never saved: the section shows a message panel instead of the controls.
    p.projectState = ProjectState::NoProject;
    ui::setText(p.repoLabel, "");
    ui::setText(p.trackLabel, "-");
    ui::setText(p.fetchTimeLabel, "-");
    return;
  }

  if (path.empty())
  {
    // Saved, but its folder is not a repository: offer to create one.
    p.projectState = ProjectState::NotARepository;
    ui::setText(p.repoLabel, util::truncate(util::fileName(projectFile), 80));
    ui::setText(p.trackLabel, "-");
    ui::setText(p.fetchTimeLabel, "-");
    return;
  }

  p.projectState = ProjectState::Ready;
  ui::setText(p.repoLabel, util::fileName(path) + "  (" + path + ")");

  git::Repo repo;
  std::string error;
  if (!repo.open(path, &error))
  {
    ui::setText(p.trackLabel, error);
    ui::setText(p.fetchTimeLabel, "-");
    return;
  }

  p.hasUncommittedChanges = repo.hasChanges(&error);

  const bool detached = repo.isHeadDetached();
  const bool unborn = repo.isHeadUnborn();
  const std::string branch = repo.currentBranch(&error);
  p.currentBranchName = detached ? std::string() : branch;

  // Combo order: the review state (only while detached), local branches,
  // remote branches. Selecting an entry checks it out; the review entry only
  // describes where HEAD is.
  int selected = -1;
  if (detached)
  {
    git::CommitInfo head;
    std::string headError;
    std::string label = "[detached]";
    if (repo.lastCommit(&head, &headError))
      label = "[" + util::truncate(util::firstLine(head.summary), 60) + "]";
    p.previewItem = label;
    p.branchItems.push_back(label);
    selected = 0;
  }
  for (const auto &name : repo.branches(&error))
  {
    if (!detached && name == branch) selected = static_cast<int>(p.branchItems.size());
    p.branchItems.push_back(name);
  }
  for (const auto &name : repo.remoteBranches(&error)) p.branchItems.push_back(name);

  for (const auto &item : p.branchItems) ui::addComboItem(p.branchCombo, item);
  if (selected >= 0 && !p.branchItems.empty()) ui::comboSelect(p.branchCombo, selected);

  if (detached)
    ui::setText(p.trackLabel, "Project on review state");
  else if (unborn)
    ui::setText(p.trackLabel, "no commits yet");
  else if (!branch.empty())
  {
    const git::AheadBehind ab = repo.aheadBehind(branch, &error);
    if (ab.hasUpstream)
      ui::setText(p.trackLabel, std::to_string(ab.ahead) + " local / " +
                                    std::to_string(ab.behind) + " server");
    else
      ui::setText(p.trackLabel, "no upstream branch");
  }
  else
  {
    ui::setText(p.trackLabel, "-");
  }

  const std::time_t fetched = repo.lastFetchTime();
  ui::setText(p.fetchTimeLabel, fetched ? util::formatTime(fetched) : "never fetched");

  // Commits that the server has and we do not (the "behind" side).
  if (!detached && !branch.empty())
  {
    const std::vector<git::CommitInfo> incoming = repo.incomingCommits(branch, 50, &error);
    for (const auto &commit : incoming)
    {
      ui::listAddRow(p.incomingList, {commit.oid, util::truncate(commit.summary, 90),
                                      commit.author, util::formatTime(commit.when)});
    }
    p.hasIncoming = !incoming.empty();
  }

  p.stashes = repo.stashes(&error);
  for (const auto &stash : p.stashes)
  {
    ui::listAddRow(p.stashList, {stash.oid, util::truncate(stash.summary, 80), stash.author,
                                 util::formatTime(stash.when), stash.branch});
  }
}

void refreshHistory(Panel &p)
{
  p.commits.clear();
  p.historyRowOffset = 0;
  p.historyRowColors.clear();
  ui::listClear(p.historyList);

  const std::string path = effectiveRepoPath(p);
  if (path.empty()) return;

  git::Repo repo;
  std::string error;
  if (!repo.open(path, &error)) return;

  // Working-tree changes are not commits, so they would otherwise be invisible
  // in the history. They get a synthetic first row (no hash/author/date).
  if (repo.hasChanges(&error))
  {
    ui::listAddRow(p.historyList, {"*", "", "[uncommited changes]", "", ""});
    p.historyRowColors.push_back(3);
    p.historyRowOffset = 1;
  }

  git::CommitInfo head;
  const bool haveHead = repo.lastCommit(&head, &error);

  // Every ref, not just HEAD's history: other branches and (while previewing)
  // commits that are not on HEAD stay visible.
  p.commits = repo.logAll(300, &error);
  for (const auto &commit : p.commits)
  {
    // 2 = current commit (blue), 1 = not on HEAD (grey), 4 = stash (yellow).
    const int color = (haveHead && commit.fullOid == head.fullOid)
                          ? 2
                          : (commit.isStash ? 4 : (commit.reachableFromHead ? 0 : 1));
    p.historyRowColors.push_back(color);

    // Best-effort graph: merges get a distinct marker. A full multi-lane graph
    // needs per-commit parent bookkeeping that libgit2's revwalk does not give
    // us directly here. ASCII only: SWELL's list font may lack unicode dots.
    const std::string graph = commit.parentCount > 1 ? "* " : "o ";
    ui::listAddRow(p.historyList, {graph, commit.oid, util::truncate(commit.summary, 90),
                                   commit.author, util::formatTime(commit.when)});
  }
}

void layout(Panel &p);

int sumHintLines(const Panel &p);

// Keeps every action button in sync with the current state, and records *why*
// a disabled button is disabled. The reasons are shown in each section's hint
// box (bold button title + reason), so the enabling rules document themselves.
void updateActionStates(Panel &p)
{
  // While a project loads, every action is locked: a git operation started
  // against a half-loaded project would act on inconsistent state.
  if (p.busy)
  {
    const HWND controls[] = {p.remotesHeader, p.reposHeader,      p.projectHeader,
                             p.historyHeader, p.stashHeader,      p.addRemote,
                             p.removeRemote,  p.cloneRepo,        p.addLocalRepo,
                             p.removeRepo,    p.openProjectButton, p.fetchButton,
                             p.recoverButton, p.pullButton,       p.pushButton,
                             p.previewButton, p.commitButton,     p.clearChangesButton,
                             p.addGitButton};
    for (HWND control : controls) ui::setEnabled(control, false);
    ui::setEnabled(p.branchCombo, false);

    setHintLines(p.remotesHint, {{"Loading:", "Wait for the project to finish loading."}});
    setHintLines(p.reposHint, {});
    setHintLines(p.projectHint, {});
    setHintLines(p.historyHint, {});
    setHintLines(p.stashHint, {});
    p.hintLinesTotal = sumHintLines(p);
    if (p.hwnd) layout(p);
    return;
  }

  const bool hasRemote = ui::listSelection(p.remoteList) >= 0;
  const bool hasRepo = ui::listSelection(p.repoList) >= 0;
  const std::string projectRepo = effectiveRepoPath(p);
  const bool hasCurrentRepo = !projectRepo.empty();

  bool hasStash = false;
  bool stashReady = false;
  std::string stashReason = "No stash entries.";
  bool canPush = false;
  std::string pushReason = "Nothing to push.";
  bool canCommit = false;
  std::string commitReason = "There are no uncommitted changes.";
  bool canPull = false;
  std::string pullReason = "Already synced with server, nothing to pull.";
  git::CommitInfo headCommit;
  bool haveHead = false;

  if (hasCurrentRepo)
  {
    git::Repo repo;
    std::string error;
    if (repo.open(projectRepo, &error))
    {
      const bool detached = repo.isHeadDetached();
      const bool dirty = repo.hasChanges(&error);
      const std::string branch = repo.currentBranch(&error);

      if (!branch.empty() && !repo.isHeadUnborn())
      {
        const git::AheadBehind ab = repo.aheadBehind(branch, &error);
        canPush = !ab.hasUpstream || ab.ahead > 0;
        pushReason = ab.hasUpstream ? "Nothing to push." : "No commits to push yet.";
        canPull = ab.hasUpstream && ab.behind > 0;
        pullReason = ab.hasUpstream ? "Already synced with server, nothing to pull."
                                    : "No upstream branch to pull from.";
      }
      else
      {
        pushReason = "The repository has no branch to push.";
        pullReason = "The repository has no branch to pull from.";
      }

      canCommit = dirty && !detached;
      commitReason = detached ? "No commit allowed during review."
                              : "There are no uncommitted changes.";

      hasStash = !repo.stashes(&error).empty();
      stashReady = hasStash && !dirty && !detached;
      if (!hasStash)
        stashReason = "No stash entries.";
      else if (detached)
        stashReason = "Leave the review state first.";
      else if (dirty)
        stashReason = "Commit or clear the current changes first.";

      haveHead = repo.lastCommit(&headCommit, &error);
    }
  }

  // Review needs a selected commit; the current commit is allowed while the
  // working tree is dirty (commit/stash moves HEAD first).
  const int historyRow = ui::listSelection(p.historyList) - p.historyRowOffset;
  bool canPreview = historyRow >= 0 && historyRow < static_cast<int>(p.commits.size());
  std::string previewReason = "Select a commit to review.";
  if (canPreview && p.commits[static_cast<std::size_t>(historyRow)].isStash)
  {
    // A stash entry is not a normal commit: checking it out would detach HEAD
    // onto a stash tree. It is recovered from the Stash section instead.
    canPreview = false;
    previewReason = "Stash entries cannot be reviewed.";
  }
  else if (canPreview && haveHead && !canCommit &&
           p.commits[static_cast<std::size_t>(historyRow)].fullOid == headCommit.fullOid)
  {
    canPreview = false;
    previewReason = "That is the current commit.";
  }

  ui::setEnabled(p.removeRemote, hasRemote);
  ui::setEnabled(p.removeRepo, hasRepo);
  ui::setEnabled(p.openProjectButton, hasRepo);
  ui::setEnabled(p.recoverButton, stashReady);
  ui::setEnabled(p.previewButton, canPreview);
  ui::setEnabled(p.commitButton, canCommit);
  ui::setEnabled(p.clearChangesButton, p.hasUncommittedChanges);
  ui::setEnabled(p.fetchButton, hasCurrentRepo);
  ui::setEnabled(p.pullButton, canPull);
  ui::setEnabled(p.pushButton, canPush);
  ui::setEnabled(p.addGitButton, p.projectState == ProjectState::NotARepository);

  // The "Clear uncommited changes" button only exists while there is
  // something to clear; that is handled by the layout.
  ui::setVisible(p.clearChangesButton, p.historyOpen && p.hasUncommittedChanges);

  // Restore the controls the busy lock disabled for the previous load.
  ui::setEnabled(p.remotesHeader, true);
  ui::setEnabled(p.reposHeader, true);
  ui::setEnabled(p.projectHeader, true);
  ui::setEnabled(p.historyHeader, true);
  ui::setEnabled(p.stashHeader, true);
  ui::setEnabled(p.branchCombo, true);
  ui::setEnabled(p.addRemote, true);
  ui::setEnabled(p.cloneRepo, true);
  ui::setEnabled(p.addLocalRepo, true);

  // Reasons for disabled buttons, grouped per section. "Select something
  // first" is not worth a hint: the empty list makes that obvious.
  auto collect =
      [](std::initializer_list<std::pair<bool, std::pair<std::string, std::string>>> entries) {
        std::vector<std::pair<std::string, std::string>> lines;
        for (const auto &entry : entries)
        {
          if (entry.first) continue;
          lines.emplace_back(entry.second.first, entry.second.second);
        }
        return lines;
      };

  setHintLines(p.remotesHint, {});
  setHintLines(p.reposHint, {});
  setHintLines(p.projectHint,
               collect({{hasCurrentRepo, {"Fetch:", "The project is not a repository."}},
                        {canPull, {"Pull:", pullReason}},
                        {canPush, {"Push:", pushReason}}}));
  setHintLines(p.historyHint,
               collect({{canPreview, {"Review:", previewReason}},
                        {canCommit, {"Commit:", commitReason}}}));
  setHintLines(p.stashHint, collect({{stashReady, {"Recover:", stashReason}}}));

  // The hint boxes shrink/grow with their content, so the layout only needs to
  // run again when the number of shown lines changes.
  const int lines = sumHintLines(p);
  if (lines != p.hintLinesTotal)
  {
    p.hintLinesTotal = lines;
    if (p.hwnd) layout(p);
  }
}

// Locks or unlocks the panel while a project loads. The status line explains
// the lock instead of the hint boxes.
void setBusy(Panel &p, bool busy)
{
  p.busy = busy;
  updateActionStates(p);
  ui::setText(p.statusLabel, busy ? "Loading project ..." : "Project loaded.");
  if (p.hwnd) ui::refreshWindow(p.hwnd);
}

void refreshAll(Panel &p)
{
  refreshRemotes(p);
  refreshRepositories(p);
  refreshProject(p);
  refreshHistory(p);
  updateActionStates(p);
  if (p.hwnd) ui::refreshWindow(p.hwnd);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

int sumHintLines(const Panel &p)
{
  // Only expanded sections reserve space for their hint box.
  int total = 0;
  if (p.remotesOpen) total += p.remotesHint.lineCount;
  if (p.reposOpen) total += p.reposHint.lineCount;
  if (p.projectOpen) total += p.projectHint.lineCount;
  if (p.historyOpen) total += p.historyHint.lineCount;
  if (p.stashOpen) total += p.stashHint.lineCount;
  return total;
}

void layout(Panel &p)
{
  if (!p.hwnd || !p.remotesHeader) return;
  RECT client{};
  GetClientRect(p.hwnd, &client);
  int width = client.right - client.left;
  if (width < 480) width = 480;

  const int x = 10;
  const int w = width - 20;
  const int rowH = 24;
  const int gap = 5;
  const int sectionGap = 12;
  int y = 8;

  auto placeRow = [&](HWND control) {
    ui::moveControl(control, x, y, w, rowH);
    y += rowH + gap;
  };

  // Collapsing a section must hide its controls, not merely stop positioning
  // them: otherwise they stay visible at their old coordinates (that is what
  // left garbage behind).
  auto showSection = [](std::initializer_list<HWND> controls, bool visible) {
    for (HWND control : controls) ui::setVisible(control, visible);
  };

  // --- Servers ----------------------------------------------------------
  placeRow(p.remotesHeader);
  if (p.remotesOpen)
  {
    ui::moveControl(p.addRemote, x, y, 110, rowH);
    layoutHintBox(p.remotesHint, x + 118, y, w - 118);
    y += std::max(rowH, hintBoxHeight(p.remotesHint)) + gap;

    ui::moveControl(p.remoteList, x, y, w, 92);
    y += 92 + gap;

    ui::moveControl(p.removeRemote, x, y, 110, rowH);
    y += rowH + sectionGap;
    showSection({p.addRemote, p.remoteList, p.removeRemote}, true);
  }
  else
  {
    showSection({p.addRemote, p.remoteList, p.removeRemote}, false);
    hideHintBox(p.remotesHint);
    y += sectionGap;
  }

  // --- Repositories -----------------------------------------------------
  placeRow(p.reposHeader);
  if (p.reposOpen)
  {
    ui::moveControl(p.addLocalRepo, x, y, 130, rowH);
    ui::moveControl(p.cloneRepo, x + 138, y, 150, rowH);
    layoutHintBox(p.reposHint, x + 296, y, w - 296);
    y += std::max(rowH, hintBoxHeight(p.reposHint)) + gap;

    ui::moveControl(p.repoList, x, y, w, 92);
    y += 92 + gap;

    ui::moveControl(p.openProjectButton, x, y, 110, rowH);
    ui::moveControl(p.removeRepo, x + 118, y, 110, rowH);
    y += rowH + sectionGap;
    showSection({p.addLocalRepo, p.cloneRepo, p.repoList, p.openProjectButton, p.removeRepo},
                true);
  }
  else
  {
    showSection({p.addLocalRepo, p.cloneRepo, p.repoList, p.openProjectButton, p.removeRepo},
                false);
    hideHintBox(p.reposHint);
    y += sectionGap;
  }

  // --- Project ----------------------------------------------------------
  placeRow(p.projectHeader);
  const HWND projectControls[] = {p.repoLabel,   p.branchLabel,  p.branchCombo,
                                  p.trackLabel,  p.fetchTimeLabel, p.fetchButton,
                                  p.pullButton,  p.pushButton,   p.incomingLabel,
                                  p.incomingList};

  if (!p.projectOpen)
  {
    for (HWND control : projectControls) ui::setVisible(control, false);
    ui::setVisible(p.projectMessage, false);
    ui::setVisible(p.addGitButton, false);
    hideHintBox(p.projectHint);
    y += sectionGap;
  }
  else if (p.projectState == ProjectState::NoProject)
  {
    for (HWND control : projectControls) ui::setVisible(control, false);
    ui::setVisible(p.addGitButton, false);
    ui::setVisible(p.projectMessage, true);
    ui::moveControl(p.projectMessage, x, y, w, 26);
    hideHintBox(p.projectHint);
    y += 26 + sectionGap;
  }
  else if (p.projectState == ProjectState::NotARepository)
  {
    for (HWND control : projectControls) ui::setVisible(control, false);
    ui::setVisible(p.projectMessage, false);
    ui::setVisible(p.addGitButton, true);
    ui::moveControl(p.addGitButton, x, y, 200, 26);
    hideHintBox(p.projectHint);
    y += 26 + sectionGap;
  }
  else
  {
    for (HWND control : projectControls) ui::setVisible(control, true);
    ui::setVisible(p.projectMessage, false);
    ui::setVisible(p.addGitButton, false);

    ui::moveControl(p.repoLabel, x, y, w, 20);
    y += 20 + gap;

    ui::moveControl(p.branchLabel, x, y, 60, rowH);
    ui::moveControl(p.branchCombo, x + 62, y, 300, ui::comboHeight());
    y += rowH + gap;

    ui::moveControl(p.trackLabel, x, y, 220, rowH);
    ui::moveControl(p.fetchTimeLabel, x + 226, y, 220, rowH);
    ui::moveControl(p.fetchButton, x + 454, y, 90, rowH);
    ui::moveControl(p.pullButton, x + 550, y, 90, rowH);
    ui::moveControl(p.pushButton, x + 646, y, 90, rowH);
    y += rowH + gap;

    layoutHintBox(p.projectHint, x, y, w);
    y += hintBoxHeight(p.projectHint) + gap;

    // "Behind" commits get their own list, only when there are any.
    ui::setVisible(p.incomingLabel, p.hasIncoming);
    ui::setVisible(p.incomingList, p.hasIncoming);
    if (p.hasIncoming)
    {
      ui::moveControl(p.incomingLabel, x, y, w, 18);
      y += 18 + gap;
      ui::moveControl(p.incomingList, x, y, w, 80);
      y += 80 + gap;
    }
    y += sectionGap;
  }

  // --- History ----------------------------------------------------------
  placeRow(p.historyHeader);
  if (p.historyOpen)
  {
    ui::moveControl(p.commitButton, x, y, 110, rowH);
    ui::moveControl(p.clearChangesButton, x + 118, y, 190, rowH);
    layoutHintBox(p.historyHint, x + 316, y, w - 316);
    y += std::max(rowH, hintBoxHeight(p.historyHint)) + gap;

    ui::moveControl(p.historyList, x, y, w, 180);
    y += 180 + gap;

    ui::moveControl(p.previewButton, x, y, 110, rowH);
    y += rowH + sectionGap;

    showSection({p.commitButton, p.historyList, p.previewButton}, true);
    // The "Clear uncommited changes" button only exists while there is
    // something to clear.
    ui::setVisible(p.clearChangesButton, p.hasUncommittedChanges);
  }
  else
  {
    showSection({p.commitButton, p.clearChangesButton, p.historyList, p.previewButton}, false);
    hideHintBox(p.historyHint);
    y += sectionGap;
  }

  // --- Stash ------------------------------------------------------------
  placeRow(p.stashHeader);
  if (p.stashOpen)
  {
    ui::moveControl(p.stashLabel, x, y, w, 18);
    y += 18 + gap;
    ui::moveControl(p.stashList, x, y, w, 92);
    y += 92 + gap;

    ui::moveControl(p.recoverButton, x, y, 110, rowH);
    layoutHintBox(p.stashHint, x + 118, y, w - 118);
    y += std::max(rowH, hintBoxHeight(p.stashHint)) + sectionGap;
    showSection({p.stashLabel, p.stashList, p.recoverButton}, true);
  }
  else
  {
    showSection({p.stashLabel, p.stashList, p.recoverButton}, false);
    hideHintBox(p.stashHint);
    y += sectionGap;
  }

  ui::moveControl(p.statusLabel, x, client.bottom - 22, w - 90, 18);
  ui::moveControl(p.versionLabel, x + w - 90, client.bottom - 22, 90, 18);

#ifdef _WIN32
  // A moved list view keeps the pixels of its old position until it is
  // invalidated, so the sections around it look dirty after a toggle.
  const HWND lists[] = {p.remoteList, p.repoList, p.incomingList, p.historyList, p.stashList};
  for (HWND list : lists)
    if (list && IsWindowVisible(list)) InvalidateRect(list, nullptr, TRUE);
#endif

  ui::refreshWindow(p.hwnd);
}

void toggleSection(Panel &p, bool &flag, HWND /*header*/, const char * /*title*/)
{
  // The header button keeps its label; its colour shows the state (see
  // WM_DRAWITEM in panelProc), so it must be repainted after the toggle.
  flag = !flag;
  layout(p);
  ui::refreshWindowDeep(p.hwnd);

  const HWND headers[] = {p.remotesHeader, p.reposHeader, p.projectHeader, p.historyHeader};
  for (HWND header : headers)
    if (header) InvalidateRect(header, nullptr, TRUE);

  ui::refreshWindow(p.hwnd);
}

void syncSectionHeaders(Panel &p)
{
  ui::setText(p.remotesHeader, "Servers");
  ui::setText(p.reposHeader, "Repositories");
  ui::setText(p.projectHeader, "Project");
  ui::setText(p.historyHeader, "History");
  ui::setText(p.stashHeader, "Stash");
}

// ---------------------------------------------------------------------------
// Remote dialog
// ---------------------------------------------------------------------------

struct RemoteDialogState
{
  std::string name;
  std::string url;
  std::string username;
  std::string secret;
  std::string passphrase;
  int auth = 0; // 0 = password, 1 = SSH key
  bool accepted = false;
  HWND panel = nullptr;
  HWND nameEdit = nullptr;
  HWND urlEdit = nullptr;
  HWND userEdit = nullptr;
  HWND authCombo = nullptr;
  HWND secretEdit = nullptr;
  HWND keyDisplay = nullptr;
  HWND keyBrowse = nullptr;
  HWND passphraseEdit = nullptr;
  HWND secretLabel = nullptr;
  HWND passphraseLabel = nullptr;
};

RemoteDialogState *g_remoteDialog = nullptr;

// Password and SSH-key authentication need different fields; only the relevant
// ones are ever shown.
void updateRemoteSecretLabel(RemoteDialogState &state)
{
  const bool ssh = state.auth == 1;

  ui::setText(state.secretLabel, ssh ? "SSH private key:" : "Password:");
  ui::setVisible(state.secretEdit, !ssh);
  ui::setVisible(state.keyDisplay, ssh);
  ui::setVisible(state.keyBrowse, ssh);
  ui::setVisible(state.passphraseLabel, ssh);
  ui::setVisible(state.passphraseEdit, ssh);
}

void buildRemoteDialog(HWND hwnd)
{
  RemoteDialogState *state = g_remoteDialog;
  if (!state) return;

  ui::addControl(hwnd, "STATIC", "Name:", 0, SS_LEFT, 14, 14, 200, 18);
  state->nameEdit = ui::addControl(hwnd, "EDIT", state->name, 99,
                                   WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 34, 520, 24);

  ui::addControl(hwnd, "STATIC", "Server URL (ssh://host:port or https://host):", 0, SS_LEFT, 14,
                 70, 400, 18);
  state->urlEdit = ui::addControl(hwnd, "EDIT", state->url, 100,
                                  WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 90, 520, 24);

  ui::addControl(hwnd, "STATIC", "Username:", 0, SS_LEFT, 14, 126, 200, 18);
  state->userEdit = ui::addControl(hwnd, "EDIT", state->username, 101,
                                   WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 146, 250, 24);

  ui::addControl(hwnd, "STATIC", "Authentication:", 0, SS_LEFT, 290, 126, 200, 18);
  state->authCombo = ui::addControl(hwnd, "COMBOBOX", "", 102,
                                    CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 290, 146, 244,
                                    ui::comboHeight());
  ui::addComboItem(state->authCombo, "Password");
  ui::addComboItem(state->authCombo, "SSH key");
  ui::comboSelect(state->authCombo, state->auth);

  state->secretLabel = ui::addControl(hwnd, "STATIC", "Password:", 0, SS_LEFT, 14, 184, 200, 18);
  state->secretEdit = ui::addControl(hwnd, "EDIT", state->secret, 103,
                                     WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 14,
                                     204, 520, 24);

  // SSH key authentication uses a file chooser instead of a text field.
  state->keyDisplay = ui::addControl(hwnd, "EDIT", state->secret, 103,
                                     WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 14, 204, 430, 24);
  state->keyBrowse = ui::addButton(hwnd, "Choose key...", 105, 452, 203, 82, 26);

  state->passphraseLabel =
      ui::addControl(hwnd, "STATIC", "SSH key passphrase (optional):", 0, SS_LEFT, 14, 242, 300, 18);
  state->passphraseEdit = ui::addControl(hwnd, "EDIT", state->passphrase, 104,
                                         WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 14,
                                         262, 250, 24);

  ui::addControl(hwnd, "STATIC",
                 "Credentials are stored in reaper_git.json in REAPER's resource folder. They are "
                 "obfuscated, not encrypted - anyone with access to that file can recover them.",
                 0, SS_LEFT, 14, 300, 520, 34);

  ui::addButton(hwnd, "Add", IDOK, 376, 348, 76, 26);
  ui::addButton(hwnd, "Cancel", IDCANCEL, 458, 348, 76, 26);

  updateRemoteSecretLabel(*state);
}

UI_DLGRET remoteDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (ui::themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (ui::isInitMessage(msg))
  {
    RemoteDialogState *state = g_remoteDialog;
    if (!state) return 0;
    ui::runBuilder(hwnd);
    ui::applyInitialSize(hwnd, "Add Server", 560, 390);
    if (state->nameEdit) SetFocus(state->nameEdit);
    return 0;
  }

  RemoteDialogState *state = g_remoteDialog;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == 102 && HIWORD(wParam) == CBN_SELCHANGE)
    {
      state->auth = ui::comboSelection(state->authCombo) == 1 ? 1 : 0;
      updateRemoteSecretLabel(*state);
      return 1;
    }
    if (id == 105)
    {
      std::string chosen = util::trim(ui::text(state->keyDisplay));
      if (ui::pickFile(hwnd, "Choose the SSH private key", chosen, chosen))
      {
        state->secret = chosen;
        ui::setText(state->keyDisplay, chosen);
      }
      return 1;
    }
    if (id == IDOK)
    {
      try
      {
        state->name = util::trim(ui::text(state->nameEdit));
        state->url = util::trim(ui::text(state->urlEdit));
        state->username = util::trim(ui::text(state->userEdit));
        state->auth = ui::comboSelection(state->authCombo) == 1 ? 1 : 0;
        state->secret = state->auth == 1 ? util::trim(ui::text(state->keyDisplay))
                                         : ui::text(state->secretEdit);
        state->passphrase = ui::text(state->passphraseEdit);

        std::string problem;
        if (state->name.empty())
          problem = "A name is required.";
        else if (state->url.empty())
          problem = "The server URL cannot be empty.";
        else if (state->auth == 1 && util::trim(state->secret).empty())
          problem = "An SSH key path is required for SSH authentication.";
        else if (state->auth == 1 && !isSshUrl(state->url))
          problem = "SSH key authentication needs an ssh:// URL (or user@host:path).";
        // SSH URLs are fine with a password: many servers allow password auth
        // over SSH, so only incompatible combinations are rejected here.

        if (problem.empty())
        {
          git::Credentials creds;
          creds.username = state->username;
          if (state->auth == 1)
          {
            creds.sshKeyPath = util::expandHome(util::trim(state->secret));
            creds.passphrase = state->passphrase;
          }
          else
          {
            creds.password = state->secret;
          }

          std::string error;
          if (!git::checkAuthentication(state->url, creds, &error))
            problem = "Could not authenticate with the git server.\n\n" + error;
        }

        if (!problem.empty())
        {
          // Keep the dialog open so the fields can be corrected.
          ui::alert(hwnd, "Add Server", problem);
          return 1;
        }
      }
      catch (const std::exception &e)
      {
        ui::alert(hwnd, "Add Server", std::string("Unexpected error: ") + e.what());
        return 1;
      }
      catch (...)
      {
        ui::alert(hwnd, "Add Server", "Unexpected error.");
        return 1;
      }

      state->accepted = true;
      EndDialog(hwnd, 1);
      return 1;
    }
    if (id == IDCANCEL)
    {
      EndDialog(hwnd, 0);
      return 1;
    }
  }
  if (msg == WM_CLOSE)
  {
    EndDialog(hwnd, 0);
    return 1;
  }
  return 0;
}

settings::Remote runAddRemoteDialog(Panel &panel, bool *ok)
{
  settings::Remote result;
  *ok = false;

  // Remember what was typed so a reopened dialog starts from there.
  static RemoteDialogState last;
  RemoteDialogState state = last;
  state.accepted = false;
  state.panel = panel.hwnd;

  g_remoteDialog = &state;
  ui::showModal(panel.hwnd, "Add Server", 560, 390, buildRemoteDialog, remoteDialogProc,
                reinterpret_cast<LPARAM>(&state));
  g_remoteDialog = nullptr;

  state.nameEdit = state.urlEdit = state.userEdit = state.authCombo = state.secretEdit =
      state.keyDisplay = state.keyBrowse = state.passphraseEdit = state.secretLabel =
          state.passphraseLabel = nullptr;
  // Remember what was typed only while the dialog is cancelled; after a server
  // is added the next dialog starts empty instead of repeating the old values.
  RemoteDialogState remembered;
  if (!state.accepted) remembered = state;
  remembered.accepted = false;
  last = remembered;

  if (!state.accepted) return result;

  // The dialog requires a name, so it is never derived here.
  result.name = util::sanitizeName(util::trim(state.name));
  result.url = util::trim(state.url);
  result.username = util::trim(state.username);
  result.auth = state.auth == 1 ? settings::AuthMethod::SshKey : settings::AuthMethod::Password;
  result.secret = state.auth == 1 ? util::trim(state.secret) : state.secret;
  result.passphrase = state.passphrase;
  *ok = true;
  return result;
}

// ---------------------------------------------------------------------------
// Clone dialog
// ---------------------------------------------------------------------------

struct CloneDialogState
{
  std::string url;
  std::string folder;
  bool accepted = false;
  HWND urlEdit = nullptr;
  HWND folderLabel = nullptr;
};

CloneDialogState *g_cloneDialog = nullptr;

void buildCloneDialog(HWND hwnd)
{
  CloneDialogState *state = g_cloneDialog;
  if (!state) return;
  ui::addControl(hwnd, "STATIC", "Remote URL:", 0, SS_LEFT, 14, 14, 200, 18);
  state->urlEdit = ui::addControl(hwnd, "EDIT", state->url, 100,
                                  WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 34, 570, 24);
  ui::addControl(hwnd, "STATIC", "Clone into:", 0, SS_LEFT, 14, 72, 200, 18);
  state->folderLabel = ui::addControl(hwnd, "STATIC", state->folder, 101,
                                      SS_LEFT | WS_BORDER, 14, 92, 460, 24);
  ui::addButton(hwnd, "Choose folder...", 103, 480, 91,
                 104, 26);
  ui::addButton(hwnd, "Clone", IDOK, 416, 136, 76, 26);
  ui::addButton(hwnd, "Cancel", IDCANCEL, 502, 136, 76, 26);
}

UI_DLGRET cloneDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (ui::themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (ui::isInitMessage(msg))
  {
    CloneDialogState *state = g_cloneDialog;
    if (!state) return 0;
    ui::runBuilder(hwnd);
    ui::applyInitialSize(hwnd, "Clone from Server", 600, 180);
    if (state->urlEdit) SetFocus(state->urlEdit);
    return 0;
  }

  CloneDialogState *state = g_cloneDialog;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == 103)
    {
      std::string chosen = state->folder;
      if (ui::pickDirectory(hwnd, "Choose the folder to clone into", state->folder, chosen))
      {
        state->folder = chosen;
        ui::setText(state->folderLabel, chosen);
      }
      return 1;
    }
    if (id == IDOK)
    {
      state->url = ui::text(state->urlEdit);
      state->accepted = true;
      EndDialog(hwnd, 1);
      return 1;
    }
    if (id == IDCANCEL)
    {
      EndDialog(hwnd, 0);
      return 1;
    }
  }
  if (msg == WM_CLOSE)
  {
    EndDialog(hwnd, 0);
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Add local repository dialog
// ---------------------------------------------------------------------------

struct AddLocalDialogState
{
  std::string folder;
  bool accepted = false;
  HWND folderLabel = nullptr;
};

AddLocalDialogState *g_addLocalDialog = nullptr;

void buildAddLocalDialog(HWND hwnd)
{
  AddLocalDialogState *state = g_addLocalDialog;
  if (!state) return;
  ui::addControl(hwnd, "STATIC", "Repository folder (must already be a git repository):", 0,
                 SS_LEFT, 14, 14, 460, 18);
  state->folderLabel = ui::addControl(hwnd, "STATIC", state->folder, 101, SS_LEFT | WS_BORDER, 14,
                                      38, 460, 24);
  ui::addButton(hwnd, "Choose folder...", 103, 480, 37, 104,
                 26);
  ui::addButton(hwnd, "Add", IDOK, 416, 90, 76, 26);
  ui::addButton(hwnd, "Cancel", IDCANCEL, 502, 90, 76, 26);
}

UI_DLGRET addLocalDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (ui::themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (ui::isInitMessage(msg))
  {
    AddLocalDialogState *state = g_addLocalDialog;
    if (!state) return 0;
    ui::runBuilder(hwnd);
    ui::applyInitialSize(hwnd, "Add from Disk", 600, 130);
    return 0;
  }

  AddLocalDialogState *state = g_addLocalDialog;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == 103)
    {
      std::string chosen = state->folder;
      if (ui::pickDirectory(hwnd, "Choose the repository folder", state->folder, chosen))
      {
        state->folder = chosen;
        ui::setText(state->folderLabel, chosen);
      }
      return 1;
    }
    if (id == IDOK)
    {
      if (state->folder.empty())
      {
        ui::alert(hwnd, "Add from Disk", "Choose a folder first.");
        return 1;
      }
      state->accepted = true;
      EndDialog(hwnd, 1);
      return 1;
    }
    if (id == IDCANCEL)
    {
      EndDialog(hwnd, 0);
      return 1;
    }
  }
  if (msg == WM_CLOSE)
  {
    EndDialog(hwnd, 0);
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void onAddRemote(Panel &p)
{
  bool ok = false;
  const settings::Remote remote = runAddRemoteDialog(p, &ok);
  if (!ok) return;

  auto &remotes = store().remotes();
  if (store().findRemote(remote.name))
  {
    int suffix = 2;
    std::string candidate = remote.name;
    while (store().findRemote(candidate)) candidate = remote.name + "-" + std::to_string(suffix++);
    settings::Remote copy = remote;
    copy.name = candidate;
    remotes.push_back(copy);
  }
  else
  {
    remotes.push_back(remote);
  }

  store().save();
  refreshRemotes(p);
  setStatus(p, "Remote added: " + remote.url);
}

void onRemoveRemote(Panel &p)
{
  const int row = ui::listSelection(p.remoteList);
  const auto &remotes = store().remotes();
  if (row < 0 || row >= static_cast<int>(remotes.size()))
  {
    ui::alert(p.hwnd, "Remove remote", "Select a remote first.");
    return;
  }

  const settings::Remote remote = remotes[static_cast<std::size_t>(row)];
  if (!ui::confirm(p.hwnd, "Remove remote",
                   "Remove the remote \"" + remote.name + "\" (" + remote.url +
                       ") from the list?"))
    return;

  store().removeRemote(remote.name);
  store().save();
  refreshRemotes(p);
  setStatus(p, "Remote removed: " + remote.name);
}

void onClone(Panel &p)
{
  CloneDialogState state;
  state.folder = repoBaseDirectory();
  g_cloneDialog = &state;
  ui::showModal(p.hwnd, "Clone from Server", 600, 180, buildCloneDialog, cloneDialogProc,
                reinterpret_cast<LPARAM>(&state));
  g_cloneDialog = nullptr;
  if (!state.accepted) return;

  const std::string url = util::trim(state.url);
  if (url.empty())
  {
    ui::alert(p.hwnd, "Clone from Server", "The remote URL cannot be empty.");
    return;
  }

  std::string folder = util::trim(state.folder);
  if (folder.empty()) folder = repoBaseDirectory();
  if (util::stripTrailingSep(folder) == util::stripTrailingSep(repoBaseDirectory()))
    folder = util::pathJoin(folder, deriveRepoName(url));
  folder = util::normalizeSlashes(folder);

  if (util::directoryExists(folder) && !git::isRepository(folder) &&
      !std::filesystem::is_empty(folder))
  {
    ui::alert(p.hwnd, "Clone from Server",
              "The target folder already exists and is not a git repository:\n" + folder);
    return;
  }

  git::Credentials creds;
  const settings::Remote *remote = store().findRemoteForUrl(url);
  if (remote)
  {
    creds.username = remote->username;
    if (remote->auth == settings::AuthMethod::SshKey)
    {
      creds.sshKeyPath = util::expandHome(remote->secret);
      creds.passphrase = remote->passphrase;
    }
    else
    {
      creds.password = remote->secret;
    }
  }

  setStatus(p, "Cloning " + url + " ...");
  std::string error;
  if (!git::clone(url, folder, creds, &error))
  {
    ui::alert(p.hwnd, "Clone from Server", "Clone failed.\n\n" + error);
    setStatus(p, "Clone failed.");
    return;
  }

  settings::Repository repo;
  repo.name = deriveRepoName(url);
  repo.path = folder;
  store().repositories().push_back(repo);
  store().currentRepository() = folder;
  store().save();

  refreshAll(p);
  setStatus(p, "Cloned into " + folder);
}

// Writes the project's .gitignore, keeping whatever it already contains. REAPER
// keeps automatic backups and the media peak cache in the project folder; both
// change on every save and must never enter the history.
bool ensureProjectGitignore(const std::string &folder, std::string *error)
{
  static const char *const kEntries[] = {"Backups/", "Media/peaks/"};

  const std::string path = util::pathJoin(folder, ".gitignore");

  std::string content;
  {
    std::ifstream in(path);
    if (in)
    {
      std::stringstream buffer;
      buffer << in.rdbuf();
      content = buffer.str();
    }
  }

  const bool created = content.empty();
  if (created) content = "# Generated by reaper_git: folders recreated on every save.\n";

  bool changed = created;
  for (const char *entry : kEntries)
  {
    // Compare whole lines, so "Backups/" is not considered present just because
    // a longer pattern happens to contain it.
    bool found = false;
    std::stringstream lines(content);
    std::string line;
    while (std::getline(lines, line))
    {
      if (util::trim(line) == entry)
      {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (!content.empty() && content.back() != '\n') content.push_back('\n');
    content += entry;
    content += '\n';
    changed = true;
  }

  if (!changed) return true;

  std::ofstream out(path, std::ios::trunc);
  if (!out)
  {
    if (error) *error = "could not write " + path;
    return false;
  }
  out << content;
  out.close();
  if (!out)
  {
    if (error) *error = "could not write " + path;
    return false;
  }
  return true;
}

// Creates a repository (git init) in the open project's folder, so history can
// be tracked locally even when no server is involved.
void onAddGit(Panel &p)
{
  if (warnIfProjectDirty(p, "creating a repository")) return;

  const std::string file = currentProjectFile();
  if (file.empty())
  {
    ui::alert(p.hwnd, "Add Git", "Save the project to disk first.");
    return;
  }

  const std::string folder = util::dirName(file);
  if (folder.empty()) return;

  std::string error;
  if (!git::initRepository(folder, &error))
  {
    ui::alert(p.hwnd, "Add Git", "Could not create the repository.\n\n" + error);
    setStatus(p, "Could not create the repository.");
    return;
  }

  // A fresh repository gets the two folders that must never be tracked: REAPER's
  // backup copies and the media peak cache. Existing lines are preserved.
  if (!ensureProjectGitignore(folder, &error))
  {
    ui::alert(p.hwnd, "Add Git", "The repository was created, but .gitignore could not be "
                                 "written.\n\n" + error);
    refreshAll(p);
    setStatus(p, "Repository created, but .gitignore could not be written.");
    return;
  }

  // Start with everything tracked, so the first commit is a usable baseline.
  git::Repo repo;
  if (!repo.open(folder, &error) || !repo.commitAll("Project setup", &error))
  {
    ui::alert(p.hwnd, "Add Git",
              "The repository was created, but the first commit failed.\n\n" + error);
    refreshAll(p);
    setStatus(p, "Repository created, but the first commit failed.");
    return;
  }

  refreshAll(p);
  setStatus(p, "Repository created and committed as \"Project setup\".");
}

void onAddLocal(Panel &p)
{
  static AddLocalDialogState last;
  AddLocalDialogState state;
  state.folder = last.folder.empty() ? store().currentRepository() : last.folder;
  state.folderLabel = nullptr;

  g_addLocalDialog = &state;
  ui::showModal(p.hwnd, "Add from Disk", 600, 130, buildAddLocalDialog, addLocalDialogProc,
                reinterpret_cast<LPARAM>(&state));
  g_addLocalDialog = nullptr;

  state.folderLabel = nullptr;
  last = state;
  if (!state.accepted) return;

  const std::string folder =
      util::stripTrailingSep(util::normalizeSlashes(util::trim(state.folder)));
  if (folder.empty()) return;

  if (!git::isRepository(folder))
  {
    ui::alert(p.hwnd, "Add from Disk", "This folder is not a git repository:\n" + folder);
    setStatus(p, "Not a git repository: " + folder);
    return;
  }

  if (store().findRepository(folder))
  {
    ui::alert(p.hwnd, "Add from Disk", "This repository is already in the list.");
    return;
  }

  settings::Repository repo;
  repo.name = util::sanitizeName(util::fileName(folder));
  repo.path = folder;
  store().repositories().push_back(repo);
  store().currentRepository() = folder;
  store().save();

  refreshAll(p);
  setStatus(p, "Repository added: " + folder);
}

void onRemoveRepo(Panel &p)
{
  const int row = ui::listSelection(p.repoList);
  const auto &repos = store().repositories();
  if (row < 0 || row >= static_cast<int>(repos.size()))
  {
    ui::alert(p.hwnd, "Remove repository", "Select a repository first.");
    return;
  }

  const settings::Repository repo = repos[static_cast<std::size_t>(row)];
  bool deleteFolder = false;
  if (!ui::confirmWithCheckbox(
          p.hwnd, "Remove repository",
          "Remove \"" + repo.name + "\" from the list?\n\n" + repo.path,
          "DANGER: Also delete repository folder from disk. THIS IS IRREVERSIBLE.", &deleteFolder))
    return;

  store().removeRepository(repo.path);
  store().save();

  if (deleteFolder)
  {
    std::error_code ec;
    std::filesystem::remove_all(repo.path, ec);
    if (ec)
      ui::alert(p.hwnd, "Remove repository",
                "The repository was removed from the list, but deleting the folder failed:\n" +
                    ec.message());
    else
      setStatus(p, "Deleted " + repo.path);
  }
  else
  {
    setStatus(p, "Repository removed from the list: " + repo.name);
  }

  refreshAll(p);
}

// Opens the REAPER project that lives in the repository root. There must be
// exactly one .rpp file directly in that folder.
void onOpenRepo(Panel &p)
{
  const int row = ui::listSelection(p.repoList);
  const auto &repos = store().repositories();
  if (row < 0 || row >= static_cast<int>(repos.size()))
  {
    ui::alert(p.hwnd, "Open project", "Select a repository first.");
    return;
  }

  const std::string repoPath = repos[static_cast<std::size_t>(row)].path;

  // Loading another project discards the current project's unsaved changes.
  if (IsProjectDirty && IsProjectDirty(nullptr) != 0)
  {
    ui::alert(p.hwnd, "Open project",
              "There are unsaved changes. Save them before opening another repository.");
    return;
  }

  std::vector<std::string> projects;
  std::error_code ec;
  std::filesystem::directory_iterator it(repoPath, ec);
  if (!ec)
  {
    for (const auto &entry : it)
    {
      std::error_code entryError;
      if (!entry.is_regular_file(entryError)) continue;
      const std::string extension = util::toLower(entry.path().extension().string());
      if (extension == ".rpp") projects.push_back(entry.path().string());
    }
  }

  std::sort(projects.begin(), projects.end());
  if (projects.size() != 1)
  {
    ui::alert(p.hwnd, "Open project",
              "There needs to be a single reaper file on the root of the repository");
    return;
  }

  const std::string project = util::normalizeSlashes(projects.front());
  setStatus(p, "Opening " + project + " ...");
  const std::string argument = "noprompt:" + project;

  // Lock the panel for the duration of the load, exactly like a reload.
  setBusy(p, true);
  Main_openProject(argument.c_str());
  setBusy(p, false);

  // Hand the foreground back so REAPER can clear its load splash.
  if (GetMainHwnd)
  {
    HWND mainWindow = GetMainHwnd();
    if (mainWindow) SetForegroundWindow(mainWindow);
  }

  // The panel is modeless, so it stays open across the project load. Refresh
  // it for the new project, and reopen it if REAPER closed it meanwhile.
  if (isOpen())
    refresh();
  else
    open();
}

bool openEffectiveRepo(Panel &p, git::Repo *repo)
{
  const std::string path = effectiveRepoPath(p);
  if (path.empty())
  {
    ui::alert(p.hwnd, "Git",
              "The open project is not inside a git repository.\n\nSave the project inside a git "
              "repository (or open one from the Repositories list) first.");
    return false;
  }
  std::string error;
  if (!repo->open(path, &error))
  {
    ui::alert(p.hwnd, "Git", "Could not open the repository.\n\n" + error);
    return false;
  }
  return true;
}

void onFetch(Panel &p)
{
  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  setStatus(p, "Fetching ...");
  std::string error;
  const git::Credentials creds = credentialsFor(repo);
  if (!repo.fetch(creds, &error))
  {
    ui::alert(p.hwnd, "Fetch", "Fetch failed.\n\n" + error);
    setStatus(p, "Fetch failed.");
    return;
  }

  refreshProject(p);
  setStatus(p, "Fetch completed.");
}

// Commits or stashes pending changes, as requested by the pull/recover/preview
// flows. Returns false when the user cancelled or the operation failed.
bool settleWorkingTree(Panel &p, git::Repo &repo, const std::string &question, bool *stashed)
{
  std::string error;
  if (!repo.hasChanges(&error)) return true;

  std::string message = "WIP";
  bool stash = false;
  if (!ui::commitOrStash(p.hwnd, "Uncommitted changes", question, &stash, &message)) return false;
  if (message.empty()) message = "WIP";

  if (stash)
  {
    // A stash taken while reviewing a commit must say where it came from, so it
    // can be told apart later in the Stash list.
    if (repo.isHeadDetached())
    {
      git::CommitInfo reviewed;
      std::string headError;
      if (repo.lastCommit(&reviewed, &headError))
        message += " (from review of \"" + util::truncate(reviewed.summary, 60) + "\")";
    }

    if (!repo.stashSave(message, &error))
    {
      ui::alert(p.hwnd, "Git", "Could not stash the changes.\n\n" + error);
      return false;
    }
    if (stashed) *stashed = true;
  }
  else
  {
    // Commits are only possible on a branch: a detached HEAD (preview) has no
    // branch to move, and the commit would be unreachable from any ref.
    if (repo.isHeadDetached())
    {
      ui::alert(p.hwnd, "Git",
                "Cannot commit while previewing a commit (detached HEAD).\n\n"
                "Choose Stash instead, or clear changes and switch to a branch first.");
      return false;
    }
    if (!repo.commitAll(message, &error))
    {
      ui::alert(p.hwnd, "Git", "Could not commit the changes.\n\n" + error);
      return false;
    }
  }
  return true;
}

void onPull(Panel &p)
{
  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  if (!settleWorkingTree(p, repo,
                         "There are changes in the repository. Commit them or stash them before "
                         "pulling?",
                         nullptr))
    return;

  setStatus(p, "Pulling ...");
  std::string error;
  const git::Credentials creds = credentialsFor(repo);
  const git::PullOutcome outcome = repo.pull(creds, &error);

  switch (outcome)
  {
    case git::PullOutcome::Failed:
      ui::alert(p.hwnd, "Pull", "Pull failed.\n\n" + error);
      setStatus(p, "Pull failed.");
      break;
    case git::PullOutcome::UpToDate:
      setStatus(p, "Already up to date.");
      break;
    case git::PullOutcome::FastForwarded:
      setStatus(p, "Pulled (fast-forward).");
      reloadCurrentProject();
      break;
    case git::PullOutcome::Merged:
      setStatus(p, "Pulled (merge commit).");
      reloadCurrentProject();
      break;
  }

  refreshAll(p);
}

// Pushes the current branch (creating it on the server and setting the
// upstream the first time, like `git push --set-upstream`).
void onPush(Panel &p)
{
  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  std::string error;
  const std::string branch = repo.currentBranch(&error);
  if (branch.empty())
  {
    ui::alert(p.hwnd, "Push", "The repository has no current branch to push.");
    return;
  }

  const git::Credentials creds = credentialsFor(repo);
  setStatus(p, "Pushing " + branch + " ...");
  if (!repo.push(branch, creds, &error))
  {
    ui::alert(p.hwnd, "Push", "Push failed.\n\n" + error);
    setStatus(p, "Push failed.");
    refreshAll(p);
    return;
  }

  refreshAll(p);
  setStatus(p, "Pushed " + branch + " to the server.");
}

bool warnIfProjectDirty(Panel &p, const char *action);

// Recover = stash pop: restores the stashed content of the files the stash
// touched (preferring the stashed version) and then drops the stash entry. The
// design never merges, so it cannot leave conflicts behind, and any failure is
// rolled back to HEAD.
void onRecover(Panel &p)
{
  const int row = ui::listSelection(p.stashList);
  if (row < 0 || row >= static_cast<int>(p.stashes.size()))
  {
    ui::alert(p.hwnd, "Recover stash", "Select a stashed commit first.");
    return;
  }

  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  if (warnIfProjectDirty(p, "recovering a stash")) return;

  std::string error;
  if (repo.isHeadDetached())
  {
    ui::alert(p.hwnd, "Recover stash",
              "Switch to a branch before recovering a stash (detached HEAD).");
    return;
  }
  if (repo.hasChanges(&error))
  {
    ui::alert(p.hwnd, "Recover stash",
              "Commit or clear the current changes before recovering a stash.");
    return;
  }

  const git::StashInfo stash = p.stashes[static_cast<std::size_t>(row)];
  setStatus(p, "Restoring stash " + stash.oid + " ...");
  if (!repo.stashPop(stash.index, &error))
  {
    const std::string message =
        error.find("conflict") != std::string::npos
            ? "This operation causes a merge conflict, which cannot be fixed with this tool yet. "
              "The operation was reversed."
            : "Could not recover the stash. The operation was reversed.\n\n" + error;
    ui::alert(p.hwnd, "Recover stash", message);
    setStatus(p, "Recover failed.");
    refreshAll(p);
    return;
  }

  reloadCurrentProject();
  refreshAll(p);
  setStatus(p, "Stash restored. Changes are not committed.");
}

// Commits the working tree to the local repository. Never pushes.
bool warnIfProjectDirty(Panel &p, const char *action);

// Discards every uncommitted change (tracked edits and untracked files).
void onClearChanges(Panel &p)
{
  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  if (warnIfProjectDirty(p, "clearing the uncommitted changes")) return;

  if (!ui::confirm(p.hwnd, "Clear changes",
                   "Discard all uncommitted changes, including untracked files?\n\n"
                   "This cannot be undone."))
    return;

  std::string error;
  bool ok = false;
  try
  {
    ok = repo.discardAllChanges(&error);
  }
  catch (const std::exception &e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "unknown error";
  }

  // Reload the project from the (now HEAD) files on disk, then refresh.
  reloadCurrentProject();
  refreshAll(p);

  if (!ok)
  {
    ui::alert(p.hwnd, "Clear changes", "Could not discard all changes.\n\n" + error);
    setStatus(p, "Clear changes failed.");
    return;
  }

  setStatus(p, "Uncommitted changes discarded.");
}

void onCommit(Panel &p)
{
  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  if (warnIfProjectDirty(p, "committing")) return;

  if (repo.isHeadDetached())
  {
    ui::alert(p.hwnd, "Commit",
              "Cannot commit while previewing a commit (detached HEAD).\n\nChoose a branch first.");
    return;
  }

  std::string error;
  if (!repo.hasChanges(&error))
  {
    setStatus(p, "Nothing to commit.");
    return;
  }

  std::string message = "Update from REAPER";
  if (!ui::prompt(p.hwnd, "Commit", "Commit message:", message)) return;
  if (message.empty()) message = "Update from REAPER";

  if (!repo.commitAll(message, &error))
  {
    ui::alert(p.hwnd, "Commit", "Could not create the commit.\n\n" + error);
    setStatus(p, "Commit failed.");
    return;
  }

  refreshAll(p);
  setStatus(p, "Committed locally: " + message);
}

// REAPER would discard the current project's unsaved edits when a checkout or
// reload changes the file on disk.
bool warnIfProjectDirty(Panel &p, const char *action)
{
  if (IsProjectDirty && IsProjectDirty(nullptr) != 0)
  {
    ui::alert(p.hwnd, "Git",
              std::string("There are unsaved changes. Save them before ") + action + ".");
    return true;
  }
  return false;
}

// Branch switch: [detached] is informational, any branch entry checks it out
// (a local branch, or a local branch tracking the selected remote branch).
//
// The branch selector is a plain push button plus our own modal chooser dialog,
// not a combo box: SWELL implements the combo popup with a native menu, and
// running a checkout + project reload from inside that popup's notification
// re-enters the menu loop and leaves the UI (and REAPER's splash) stuck.
void onBranchChanged(Panel &p, int index)
{
  if (index < 0 || index >= static_cast<int>(p.branchItems.size())) return;

  const std::string item = p.branchItems[static_cast<std::size_t>(index)];
  // The detached-HEAD entry describes where we are; it is not a branch.
  if (!p.previewItem.empty() && item == p.previewItem) return;
  // Selecting the branch we are already on is a no-op.
  if (item == p.currentBranchName) return;

  if (warnIfProjectDirty(p, "checking out a branch"))
  {
    refreshProject(p);
    return;
  }

  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  std::string error;
  // Same rule as preview: the working tree must be settled before switching.
  if (!settleWorkingTree(p, repo,
                         "There are changes in the repository. Commit them or stash them before "
                         "switching to " +
                             item + "?",
                         nullptr))
  {
    refreshProject(p);
    return;
  }

  setStatus(p, "Checking out " + item + " ...");
  const bool remote = item.find('/') != std::string::npos;
  const bool ok = remote ? repo.checkoutRemoteBranch(item, &error)
                         : repo.checkoutBranch(item, &error);
  if (!ok)
  {
    ui::alert(p.hwnd, "Checkout", "Could not check out " + item + ".\n\n" + error);
    refreshProject(p);
    return;
  }

  reloadCurrentProject();
  refreshAll(p);
  setStatus(p, "Checked out " + item + ".");
}

void onPreview(Panel &p)
{
  // Row 0 may be the synthetic "[UNCOMMITED CHANGES]" entry.
  const int row = ui::listSelection(p.historyList) - p.historyRowOffset;
  if (row < 0 || row >= static_cast<int>(p.commits.size()))
  {
    ui::alert(p.hwnd, "Review", "Select a commit first.");
    return;
  }

  const git::CommitInfo commit = p.commits[static_cast<std::size_t>(row)];
  if (commit.isStash)
  {
    ui::alert(p.hwnd, "Review",
              "Stash entries cannot be reviewed.\n\nUse Recover in the Stash section instead.");
    return;
  }

  git::Repo repo;
  if (!openEffectiveRepo(p, &repo)) return;

  if (warnIfProjectDirty(p, "previewing a commit")) return;

  std::string error;
  git::CommitInfo head;
  const bool dirty = repo.hasChanges(&error);

  // Previewing HEAD is allowed while there are uncommitted changes: the
  // commit/stash step moves HEAD first, so the target is meaningful again.
  if (!dirty && repo.lastCommit(&head, &error) && head.fullOid == commit.fullOid)
  {
    ui::alert(p.hwnd, "Review", "That is the current commit.");
    return;
  }

  if (dirty && !settleWorkingTree(p, repo,
                                  "There are changes in the repository. Commit them or stash them "
                                  "before previewing commit " +
                                      commit.oid + "?",
                                  nullptr))
    return;

  // Settling may have committed (HEAD moved) or stashed (HEAD unchanged). If
  // HEAD is still the selected commit there is nothing to check out, but the
  // working tree did change (the stash reverted the files), so reload anyway.
  git::CommitInfo headAfter;
  if (repo.lastCommit(&headAfter, &error) && headAfter.fullOid == commit.fullOid)
  {
    reloadCurrentProject();
    refreshAll(p);
    setStatus(p, "The repository is already at " + commit.oid + ".");
    return;
  }

  setStatus(p, "Checking out " + commit.oid + " ...");
  if (!repo.checkoutCommit(commit.fullOid, &error))
  {
    ui::alert(p.hwnd, "Review", "Could not check out the commit.\n\n" + error);
    setStatus(p, "Checkout failed.");
    return;
  }

  reloadCurrentProject();
  refreshAll(p);
  setStatus(p, "Reviewing commit " + commit.oid + ".");
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

void buildPanel(HWND hwnd)
{
  Panel *p = g_panel;
  if (!p) return;

  // Small fonts for the action hints; created once per session.
  if (!g_hintFont) g_hintFont = ui::createFont(11, false);
  if (!g_hintBoldFont) g_hintBoldFont = ui::createFont(11, true);
  if (!g_boldFont) g_boldFont = ui::createFont(13, true);

  // --- Servers (stored as "remotes") ------------------------------------
  p->remotesHeader = ui::addControl(hwnd, "BUTTON", "Servers", IDC_SEC_REMOTES,
                                    BS_OWNERDRAW | WS_TABSTOP, 10, 10, 100, 24);
  p->addRemote = ui::addControl(hwnd, "BUTTON", "Add Server", IDC_REMOTE_ADD,
                                BS_OWNERDRAW | WS_TABSTOP, 10, 40, 110, 24);
  p->remoteList = ui::addControl(hwnd, "SysListView32", "", IDC_REMOTE_LIST,
                                 LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
                                     WS_TABSTOP,
                                 10, 70, 100, 92);
  ui::listSelectFullRows(p->remoteList);
  ui::listAddColumn(p->remoteList, 0, "Name", 140);
  ui::listAddColumn(p->remoteList, 1, "URL", 380);
  ui::listAddColumn(p->remoteList, 2, "User", 120);
  ui::listAddColumn(p->remoteList, 3, "Auth", 90);
  p->removeRemote = ui::addControl(hwnd, "BUTTON", "Remove", IDC_REMOTE_REMOVE,
                                   BS_OWNERDRAW | WS_TABSTOP, 10, 170, 110, 24);
  createHintBox(hwnd, p->remotesHint);

  // --- Repositories ------------------------------------------------------
  p->reposHeader = ui::addControl(hwnd, "BUTTON", "Repositories", IDC_SEC_REPOS,
                                  BS_OWNERDRAW | WS_TABSTOP, 10, 210, 100, 24);
  p->addLocalRepo = ui::addControl(hwnd, "BUTTON", "Add from Disk", IDC_REPO_ADD,
                                   BS_OWNERDRAW | WS_TABSTOP, 10, 240, 130, 24);
  p->cloneRepo = ui::addControl(hwnd, "BUTTON", "Clone from Server", IDC_REPO_CLONE,
                                BS_OWNERDRAW | WS_TABSTOP, 148, 240, 150, 24);
  p->repoList = ui::addControl(hwnd, "SysListView32", "", IDC_REPO_LIST,
                               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
                                   WS_TABSTOP,
                               10, 270, 100, 92);
  ui::listSelectFullRows(p->repoList);
  ui::listAddColumn(p->repoList, 0, "Remote", 140);
  ui::listAddColumn(p->repoList, 1, "Name", 200);
  ui::listAddColumn(p->repoList, 2, "Folder", 560);
  p->openProjectButton = ui::addControl(hwnd, "BUTTON", "Open", IDC_REPO_OPEN,
                                       BS_OWNERDRAW | WS_TABSTOP, 10, 370, 110, 24);
  p->removeRepo = ui::addControl(hwnd, "BUTTON", "Remove", IDC_REPO_REMOVE,
                                 BS_OWNERDRAW | WS_TABSTOP, 128, 370, 110, 24);
  createHintBox(hwnd, p->reposHint);

  // --- Project -----------------------------------------------------------
  p->projectHeader = ui::addControl(hwnd, "BUTTON", "Project", IDC_SEC_PROJECT,
                                    BS_OWNERDRAW | WS_TABSTOP, 10, 410, 100, 24);
  p->repoLabel = ui::addControl(hwnd, "STATIC", "No repository selected", 0, SS_LEFT, 10, 440, 100,
                                20);
  ui::setFont(p->repoLabel, g_boldFont);
  p->projectMessage = ui::addControl(hwnd, "STATIC", "Project not saved to disk.", 0,
                                     SS_LEFT | SS_CENTERIMAGE, 10, 464, 100, 26);
  p->addGitButton = ui::addControl(hwnd, "BUTTON", "Add Git to this Project", IDC_ADD_GIT,
                                   BS_OWNERDRAW | WS_TABSTOP, 10, 464, 200, 26);
  p->branchLabel = ui::addControl(hwnd, "STATIC", "Branch:", 0, SS_LEFT, 10, 496, 60, 24);
  p->branchCombo = ui::addControl(hwnd, "COMBOBOX", "", IDC_BRANCH_COMBO,
                                  CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 72, 496, 300,
                                  ui::comboHeight());
  p->trackLabel = ui::addControl(hwnd, "STATIC", "-", 0, SS_LEFT, 10, 526, 220, 24);
  p->fetchTimeLabel = ui::addControl(hwnd, "STATIC", "never fetched", 0, SS_LEFT, 236, 526, 210,
                                     24);
  p->fetchButton = ui::addControl(hwnd, "BUTTON", "Fetch", IDC_FETCH, BS_OWNERDRAW | WS_TABSTOP,
                                  454, 526, 90, 24);
  p->pullButton = ui::addControl(hwnd, "BUTTON", "Pull", IDC_PULL, BS_OWNERDRAW | WS_TABSTOP, 550,
                                 526, 90, 24);
  p->pushButton = ui::addControl(hwnd, "BUTTON", "Push", IDC_PUSH, BS_OWNERDRAW | WS_TABSTOP, 646,
                                 526, 90, 24);
  p->incomingLabel = ui::addControl(hwnd, "STATIC", "Commits on the server that are not local:",
                                    0, SS_LEFT, 10, 556, 400, 18);
  p->incomingList = ui::addControl(hwnd, "SysListView32", "", IDC_INCOMING_LIST,
                                   LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                                   10, 576, 100, 80);
  ui::listSelectFullRows(p->incomingList);
  ui::listAddColumn(p->incomingList, 0, "Hash", 80);
  ui::listAddColumn(p->incomingList, 1, "Message", 470);
  ui::listAddColumn(p->incomingList, 2, "Author", 130);
  ui::listAddColumn(p->incomingList, 3, "Date", 130);
  createHintBox(hwnd, p->projectHint);

  // --- History -----------------------------------------------------------
  p->historyHeader = ui::addControl(hwnd, "BUTTON", "History", IDC_SEC_HISTORY,
                                    BS_OWNERDRAW | WS_TABSTOP, 10, 680, 100, 24);
  p->commitButton = ui::addControl(hwnd, "BUTTON", "Commit", IDC_COMMIT, BS_OWNERDRAW | WS_TABSTOP,
                                   10, 710, 110, 24);
  p->clearChangesButton = ui::addControl(hwnd, "BUTTON", "Clear uncommited changes",
                                         IDC_CLEAR_CHANGES, BS_OWNERDRAW | WS_TABSTOP, 128, 710,
                                         190, 24);
  p->historyList = ui::addControl(hwnd, "SysListView32", "", IDC_HISTORY_LIST,
                                  LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
                                      WS_TABSTOP,
                                  10, 740, 100, 160);
  ui::listSelectFullRows(p->historyList);
  ui::listAddColumn(p->historyList, 0, "", 40);
  ui::listAddColumn(p->historyList, 1, "Hash", 75);
  ui::listAddColumn(p->historyList, 2, "Message", 470);
  ui::listAddColumn(p->historyList, 3, "Author", 130);
  ui::listAddColumn(p->historyList, 4, "Date", 130);
  p->previewButton = ui::addControl(hwnd, "BUTTON", "Review", IDC_PREVIEW,
                                    BS_OWNERDRAW | WS_TABSTOP, 10, 910, 110, 24);
  createHintBox(hwnd, p->historyHint);

  // --- Stash -------------------------------------------------------------
  p->stashHeader = ui::addControl(hwnd, "BUTTON", "Stash", IDC_SEC_STASH,
                                  BS_OWNERDRAW | WS_TABSTOP, 10, 950, 100, 24);
  p->stashLabel = ui::addControl(hwnd, "STATIC", "Stashed changes:", 0, SS_LEFT, 10, 980, 200, 18);
  p->stashList = ui::addControl(hwnd, "SysListView32", "", IDC_STASH_LIST,
                                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
                                    WS_TABSTOP,
                                10, 1000, 100, 92);
  ui::listSelectFullRows(p->stashList);
  ui::listAddColumn(p->stashList, 0, "Hash", 70);
  ui::listAddColumn(p->stashList, 1, "Message", 420);
  ui::listAddColumn(p->stashList, 2, "Author", 120);
  ui::listAddColumn(p->stashList, 3, "Date", 130);
  ui::listAddColumn(p->stashList, 4, "Branch", 120);
  p->recoverButton = ui::addControl(hwnd, "BUTTON", "Recover", IDC_RECOVER,
                                    BS_OWNERDRAW | WS_TABSTOP, 10, 1100, 110, 24);
  createHintBox(hwnd, p->stashHint);

  p->statusLabel = ui::addControl(hwnd, "STATIC", "Ready.", IDC_STATUS, SS_LEFT, 10, 1140, 100, 18);
  p->versionLabel = ui::addControl(hwnd, "STATIC", "v" REAPER_GIT_VERSION, 0, SS_RIGHT, 10, 1140,
                                   100, 18);
}

UI_DLGRET panelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (ui::isInitMessage(msg))
  {
    Panel *p = g_panel;
    if (!p) return 0;
    p->hwnd = hwnd;
    ui::runBuilder(hwnd);
    ui::applyInitialSize(hwnd, "Git", 960, 880);

    syncSectionHeaders(*p);
    layout(*p);
    refreshAll(*p);
    return 0;
  }

  Panel *p = g_panel;
  if (!p || p->hwnd != hwnd) return 0;

  switch (msg)
  {
    case WM_SIZE:
      layout(*p);
      return 0; // let SWELL's default dialog proc do its own bookkeeping

    // Dark chrome: the panel paints its own background and text colours.
    case WM_ERASEBKGND:
    {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT client{};
      GetClientRect(hwnd, &client);
      ui::fillRect(dc, client, ui::brush(kPanelBackground));
      return 1;
    }

    case WM_CTLCOLORDLG:
    {
      HDC dc = reinterpret_cast<HDC>(wParam);
      if (dc)
      {
        SetBkColor(dc, kPanelBackground);
        SetTextColor(dc, kTextOnDark);
      }
      return reinterpret_cast<LRESULT>(ui::brush(kPanelBackground));
    }

    case WM_CTLCOLORSTATIC:
    {
      HDC dc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);

      // "Project not saved to disk." gets its own light-yellow panel.
      if (control && control == p->projectMessage)
      {
        if (dc)
        {
          SetBkColor(dc, kMessageBackground);
          SetTextColor(dc, RGB(0, 0, 0));
        }
        return reinterpret_cast<LRESULT>(ui::brush(kMessageBackground));
      }

      // Hint text sits on top of the hint box: no background of its own.
      const HintBox *hintBoxes[] = {&p->remotesHint, &p->reposHint, &p->projectHint,
                                    &p->historyHint, &p->stashHint};
      for (const HintBox *hint : hintBoxes)
      {
        for (const HintLine &line : hint->lines)
        {
          if (control && (control == line.title || control == line.reason))
          {
#ifdef _WIN32
            // The hint text sits on the hint box, so it needs that colour as
            // background; SWELL's "no background" marker is not a valid brush
            // here and left the label blank.
            if (dc)
            {
              SetBkColor(dc, kSectionClosed);
              SetTextColor(dc, kTextOnDark);
            }
            return reinterpret_cast<LRESULT>(ui::brush(kSectionClosed));
#else
            if (dc)
            {
              SetBkMode(dc, TRANSPARENT);
              SetTextColor(dc, kTextOnDark);
            }
            // (HBRUSH)1 tells SWELL "draw no background for this control".
            return 1;
#endif
          }
        }
      }

      if (dc)
      {
        SetBkColor(dc, kPanelBackground);
        SetTextColor(dc, kTextOnDark);
      }
      return reinterpret_cast<LRESULT>(ui::brush(kPanelBackground));
    }

    // Section headers, action buttons and hint boxes are owner-drawn: SWELL
    // does not theme individual controls, so the panel paints them itself.
    case WM_DRAWITEM:
    {
      auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
      if (!item || item->CtlType != ODT_BUTTON) return 0;

      // Hint box frames: a flat panel in the collapsed-section grey.
      const HWND hintBoxes[] = {p->remotesHint.box, p->reposHint.box, p->projectHint.box,
                                p->historyHint.box, p->stashHint.box};
      for (HWND hintBox : hintBoxes)
      {
        if (hintBox && item->hwndItem == hintBox)
        {
          ui::fillRect(item->hDC, item->rcItem, ui::brush(kSectionClosed));
          return 1;
        }
      }

      // Section headers: colour shows whether the section is expanded.
      bool expanded = true;
      switch (item->CtlID)
      {
        case IDC_SEC_REMOTES: expanded = p->remotesOpen; break;
        case IDC_SEC_REPOS: expanded = p->reposOpen; break;
        case IDC_SEC_PROJECT: expanded = p->projectOpen; break;
        case IDC_SEC_HISTORY: expanded = p->historyOpen; break;
        case IDC_SEC_STASH: expanded = p->stashOpen; break;
        case IDC_REMOTE_ADD:
        case IDC_REMOTE_REMOVE:
        case IDC_REPO_CLONE:
        case IDC_REPO_ADD:
        case IDC_REPO_REMOVE:
        case IDC_REPO_OPEN:
        case IDC_FETCH:
        case IDC_PULL:
        case IDC_PUSH:
        case IDC_RECOVER:
        case IDC_PREVIEW:
        case IDC_COMMIT:
        case IDC_CLEAR_CHANGES:
        case IDC_ADD_GIT:
        {
          // Action buttons: light face with black text; greyed out when
          // disabled so the state stays obvious.
          const bool enabled = IsWindowEnabled(item->hwndItem) != 0;
          ui::fillRect(item->hDC, item->rcItem,
                       ui::brush(enabled ? kButtonFace : kButtonFaceDisabled));
          SetTextColor(item->hDC, enabled ? kButtonText : kButtonTextDisabled);
          ui::drawCenteredText(item->hDC, item->rcItem, item->hwndItem);
          return 1;
        }
        default:
          // Unknown owner-drawn control: draw it as a plain button rather than
          // leaving it invisible.
          ui::fillRect(item->hDC, item->rcItem, ui::brush(kSectionClosed));
          SetTextColor(item->hDC, kTextOnDark);
          ui::drawCenteredText(item->hDC, item->rcItem, item->hwndItem);
          return 1;
      }

      ui::fillRect(item->hDC, item->rcItem, ui::brush(expanded ? kSectionOpen : kSectionClosed));
      SetTextColor(item->hDC, expanded ? kTextOnTeal : kTextOnDark);
      ui::drawCenteredText(item->hDC, item->rcItem, item->hwndItem);
      return 1;
    }

    case WM_COMMAND:
    {
      const int id = LOWORD(wParam);

      // The panel is locked while a project loads: ignore every command.
      if (p->busy) return 1;

      // Branch combo: choosing a branch checks it out.
      if (id == IDC_BRANCH_COMBO && HIWORD(wParam) == CBN_SELCHANGE)
      {
        try
        {
          onBranchChanged(*p, ui::comboSelection(p->branchCombo));
        }
        catch (const std::exception &e)
        {
          ui::alert(hwnd, "Git", std::string("Unexpected error: ") + e.what());
        }
        catch (...)
        {
          ui::alert(hwnd, "Git", "Unexpected error.");
        }
        updateActionStates(*p);
        return 1;
      }

      // Nothing may write to disk (repository or settings) while the open
      // project has unsaved changes. Actions with their own specific wording
      // (Open, Preview, Commit) do this check themselves.
      const bool mutating = id == IDC_REMOTE_ADD || id == IDC_REMOTE_REMOVE ||
                            id == IDC_REPO_CLONE || id == IDC_REPO_ADD ||
                            id == IDC_REPO_REMOVE || id == IDC_FETCH || id == IDC_PULL ||
                            id == IDC_PUSH || id == IDC_RECOVER || id == IDC_CLEAR_CHANGES;
      if (mutating && warnIfProjectDirty(*p, "running a git operation"))
      {
        updateActionStates(*p);
        return 1;
      }

      // One place that turns any escaped C++ exception into a message box, so
      // a failing action can never leave the panel (or a dialog) wedged.
      try
      {
        switch (id)
        {
          case IDC_SEC_REMOTES:
            toggleSection(*p, p->remotesOpen, p->remotesHeader, "Servers");
            break;
          case IDC_SEC_REPOS:
            toggleSection(*p, p->reposOpen, p->reposHeader, "Repositories");
            break;
          case IDC_SEC_PROJECT:
            toggleSection(*p, p->projectOpen, p->projectHeader, "Project");
            break;
          case IDC_SEC_HISTORY:
            toggleSection(*p, p->historyOpen, p->historyHeader, "History");
            break;
          case IDC_SEC_STASH:
            toggleSection(*p, p->stashOpen, p->stashHeader, "Stash");
            break;
          case IDC_ADD_GIT:
            onAddGit(*p);
            break;
          case IDC_REMOTE_ADD:
            onAddRemote(*p);
            break;
          case IDC_REMOTE_REMOVE:
            onRemoveRemote(*p);
            break;
          case IDC_REPO_CLONE:
            onClone(*p);
            break;
          case IDC_REPO_ADD:
            onAddLocal(*p);
            break;
          case IDC_REPO_REMOVE:
            onRemoveRepo(*p);
            break;
          case IDC_REPO_OPEN:
            onOpenRepo(*p);
            break;
          case IDC_FETCH:
            onFetch(*p);
            break;
          case IDC_PULL:
            onPull(*p);
            break;
          case IDC_PUSH:
            onPush(*p);
            break;
          case IDC_RECOVER:
            onRecover(*p);
            break;
          case IDC_PREVIEW:
            onPreview(*p);
            break;
          case IDC_COMMIT:
            onCommit(*p);
            break;
          case IDC_CLEAR_CHANGES:
            onClearChanges(*p);
            break;
          default:
            break;
        }
      }
      catch (const std::exception &e)
      {
        ui::alert(hwnd, "Git", std::string("Unexpected error: ") + e.what());
      }
      catch (...)
      {
        ui::alert(hwnd, "Git", "Unexpected error.");
      }
      updateActionStates(*p);
      return 1;
    }

    case WM_NOTIFY:
    {
      auto *hdr = reinterpret_cast<NMHDR *>(lParam);

      // Per-row text colours for the history: SWELL honours clrText in
      // NM_CUSTOMDRAW (it has no per-item background support).
      if (hdr && hdr->idFrom == IDC_HISTORY_LIST && hdr->code == NM_CUSTOMDRAW)
      {
        auto *custom = reinterpret_cast<NMLVCUSTOMDRAW *>(lParam);
#ifdef _WIN32
        // Win32 asks for the whole control first and for each row afterwards,
        // and expects the answer in DWLP_MSGRESULT rather than as the return
        // value of a dialog procedure.
        if (custom->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
          SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
          return 1;
        }
        if (custom->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return 1;
#endif
        const int row = static_cast<int>(custom->nmcd.dwItemSpec);
        if (row >= 0 && row < static_cast<int>(p->historyRowColors.size()))
        {
          switch (p->historyRowColors[static_cast<std::size_t>(row)])
          {
            case 1: custom->clrText = RGB(120, 120, 120); break;  // not on HEAD
            case 2: custom->clrText = RGB(30, 90, 200); break;    // current commit
            case 3: custom->clrText = RGB(170, 130, 0); break;    // uncommitted changes
            case 4: custom->clrText = RGB(170, 130, 0); break;    // stash entry
            default: break;
          }
        }
#ifdef _WIN32
        SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NEWFONT);
#endif
        return 1;
      }

      // Selection changes only affect which buttons are usable. The Project
      // and History sections deliberately stay tied to the open project.
      updateActionStates(*p);
      return 0;
    }

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 1;

    case WM_DESTROY:
      p->hwnd = nullptr;
      return 1;

    default:
      break;
  }
  return 0;
}

} // namespace

void open()
{
  if (g_panel && g_panel->hwnd)
  {
    SetForegroundWindow(g_panel->hwnd);
    return;
  }

  if (!g_panel) g_panel = new Panel();
  store().ensureLoaded();

  // An SSH server that is not in this machine's known_hosts file (always the
  // case on Windows, which has no OpenSSH configuration) is trusted once the
  // user confirms its fingerprint; the key is then remembered in the settings.
  git::setHostKeyPrompt([](const std::string &host, const std::string &fingerprint) {
    return ui::confirm(g_panel ? g_panel->hwnd : nullptr, "Unknown SSH Host",
                       "The SSH host key of \"" + host + "\" is not trusted yet.\n\n" +
                           fingerprint +
                           "\n\nAccept this key and remember it for this host?\n"
                           "Only accept it if it belongs to the server you expect.");
  });
  HWND parent = GetMainHwnd ? GetMainHwnd() : nullptr;
  HWND hwnd = ui::createWindow(parent, "Git", 960, 880, buildPanel, panelProc, 0);
  if (hwnd)
  {
    layout(*g_panel);
    refresh();
    // Deep repaint on open: the controls were created while the window was being
    // shown, and a couple of them would otherwise keep an unpainted corner.
    ui::refreshWindowDeep(hwnd);
  }
}

bool isOpen() { return g_panel && g_panel->hwnd; }

void close()
{
  if (g_panel && g_panel->hwnd) DestroyWindow(g_panel->hwnd);
  delete g_panel;
  g_panel = nullptr;
}

void refresh()
{
  if (g_panel && g_panel->hwnd)
  {
    g_lastProjectFile = currentProjectFile();
    g_lastProjectDirty = IsProjectDirty ? IsProjectDirty(nullptr) : 0;
    refreshAll(*g_panel);
  }
}

void pollProjectChange() { refreshProjectIfChanged(); }

void setStatus(const std::string &message)
{
  if (g_panel && g_panel->hwnd) setStatus(*g_panel, message);
}

} // namespace panel
