// Repo::logAll - history across every ref, used by the panel's History list.
//
// Kept in its own translation unit: it needs <unordered_set> and a slightly
// different walk than Repo::log(), which only follows HEAD.
#include "git_service.h"

#include <algorithm>
#include <cstddef>
#include <unordered_set>

#include <git2.h>

#include "util.h"

namespace git
{

namespace
{

std::string oidToString(const git_oid *oid, std::size_t maxChars)
{
  if (!oid) return {};
  char buf[GIT_OID_MAX_HEXSIZE + 1] = {0};
  git_oid_tostr(buf, sizeof(buf), oid);
  std::string out(buf);
  if (maxChars && out.size() > maxChars) out.resize(maxChars);
  return out;
}

void setError(std::string *error, const std::string &message)
{
  if (error) *error = message;
}

void setLastError(std::string *error, const char *context)
{
  const git_error *err = git_error_last();
  const std::string detail = err && err->message ? err->message : "";
  setError(error, detail.empty() ? std::string(context) : std::string(context) + ": " + detail);
}

// Stash entries and their helper commits (^2 = index, ^3 = untracked files).
struct StashScan
{
  git_repository *repo = nullptr;
  std::unordered_set<std::string> commits;
  std::unordered_set<std::string> extras;
};

int collectStashEntry(std::size_t, const char *, const git_oid *oid, void *payload)
{
  auto *scan = static_cast<StashScan *>(payload);
  if (!scan || !scan->repo || !oid) return 0;

  scan->commits.insert(oidToString(oid, 0));

  git_commit *commit = nullptr;
  if (git_commit_lookup(&commit, scan->repo, oid) != 0) return 0;

  const unsigned int parents = git_commit_parentcount(commit);
  for (unsigned int i = 1; i < parents; ++i)
  {
    git_commit *parent = nullptr;
    if (git_commit_parent(&parent, commit, i) == 0)
    {
      scan->extras.insert(oidToString(git_commit_id(parent), 0));
      git_commit_free(parent);
    }
  }
  git_commit_free(commit);
  return 0;
}

// libgit2 writes stash summaries as "On <branch>: <message>"; show them as
// "Stash: <message>" instead.
std::string stashSummary(const std::string &summary)
{
  if (summary.rfind("On ", 0) == 0)
  {
    const std::size_t colon = summary.find(": ");
    if (colon != std::string::npos) return "Stash: " + summary.substr(colon + 2);
  }
  if (summary.rfind("WIP on ", 0) == 0) return "Stash";
  return "Stash: " + summary;
}

} // namespace

std::vector<CommitInfo> Repo::logAll(std::size_t maxCount, std::string *error) const
{
  std::vector<CommitInfo> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }

  // Collect everything reachable from HEAD first, for colouring.
  std::unordered_set<std::string> headOids;
  {
    git_revwalk *headWalk = nullptr;
    if (git_revwalk_new(&headWalk, repo_) == 0)
    {
      git_revwalk_sorting(headWalk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
      if (git_revwalk_push_head(headWalk) == 0)
      {
        git_oid oid;
        while (git_revwalk_next(&oid, headWalk) == 0) headOids.insert(oidToString(&oid, 0));
      }
      git_revwalk_free(headWalk);
    }
  }

  // Stash entries are shown (as "Stash: ..."), their helper commits are hidden.
  StashScan stash;
  stash.repo = repo_;
  git_stash_foreach(repo_, collectStashEntry, &stash);

  git_revwalk *walk = nullptr;
  if (git_revwalk_new(&walk, repo_) != 0)
  {
    setLastError(error, "could not walk the history");
    return out;
  }
  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

  bool pushed = git_revwalk_push_glob(walk, "refs/*") == 0;
  // HEAD may be detached and not referenced by any branch.
  if (git_revwalk_push_head(walk) == 0) pushed = true;
  if (!pushed)
  {
    git_revwalk_free(walk);
    setLastError(error, "could not read the references");
    return out;
  }

  git_oid oid;
  while (out.size() < maxCount && git_revwalk_next(&oid, walk) == 0)
  {
    const std::string full = oidToString(&oid, 0);
    const bool isStash = stash.commits.find(full) != stash.commits.end();

    // "index on ..." / untracked helper commits are implementation noise.
    if (!isStash && stash.extras.find(full) != stash.extras.end()) continue;

    git_commit *commit = nullptr;
    if (git_commit_lookup(&commit, repo_, &oid) != 0) continue;

    CommitInfo info;
    info.fullOid = full;
    info.oid = oidToString(&oid, 8);
    const char *summary = git_commit_summary(commit);
    info.summary = summary ? summary : "";
    info.isStash = isStash;
    if (isStash) info.summary = stashSummary(info.summary);
    info.parentCount = static_cast<int>(git_commit_parentcount(commit));
    info.reachableFromHead = headOids.find(info.fullOid) != headOids.end();
    const git_signature *author = git_commit_author(commit);
    if (author)
    {
      info.author = author->name ? author->name : "";
      info.when = author->when.time;
    }
    out.push_back(std::move(info));
    git_commit_free(commit);
  }

  git_revwalk_free(walk);
  return out;
}

} // namespace git
