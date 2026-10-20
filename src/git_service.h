// Thin RAII wrapper around libgit2 for the operations the panel needs.
#pragma once

#include <ctime>
#include <string>
#include <vector>

struct git_repository;

namespace git
{

struct Credentials
{
  std::string username;
  std::string password;   // password auth
  std::string sshKeyPath; // SSH auth: path to the private key
  std::string passphrase; // SSH auth: optional passphrase
};

struct CommitInfo
{
  std::string oid;     // abbreviated
  std::string fullOid; // hex
  std::string summary;
  std::string author;
  std::time_t when = 0;
  int parentCount = 0;
  bool reachableFromHead = false;
  bool isStash = false;
};

struct StashInfo
{
  std::size_t index = 0;
  std::string oid;
  std::string summary;
  std::string branch;
  std::string author;
  std::time_t when = 0;
};

struct AheadBehind
{
  int ahead = 0;
  int behind = 0;
  bool hasUpstream = false;
};

// Result of the merge analysis used by pull().
enum class PullOutcome
{
  Failed,
  UpToDate,
  FastForwarded,
  Merged,
};

void init();
void shutdown();

bool isRepository(const std::string &path);
// Creates a new repository (git init) in `path`; used by "Add Git to this
// Project" when the opened project is not tracked yet.
bool initRepository(const std::string &path, std::string *error);
bool clone(const std::string &url, const std::string &destPath, const Credentials &creds,
           std::string *error);

// Connects to `url` (anonymous remote) and lists refs to verify URL+credentials.
bool checkRemote(const std::string &url, const Credentials &creds, std::string *error);

// Validates the credentials against a *server* URL (e.g. ssh://host:port or
// https://host) without needing a repository: for SSH the connection performs
// the full authentication handshake. HTTP(S) servers cannot authenticate
// without a repository path, so only an explicit auth rejection counts as a
// failure there.
bool checkAuthentication(const std::string &url, const Credentials &creds, std::string *error);

class Repo
{
public:
  Repo() = default;
  ~Repo();
  Repo(const Repo &) = delete;
  Repo &operator=(const Repo &) = delete;
  Repo(Repo &&other) noexcept;
  Repo &operator=(Repo &&other) noexcept;

  bool open(const std::string &path, std::string *error);
  bool isOpen() const { return repo_ != nullptr; }
  std::string workdir() const;
  std::string remoteUrl(std::string *error) const;

  std::string currentBranch(std::string *error) const;
  bool isHeadDetached() const;
  bool isHeadUnborn() const;
  bool hasChanges(std::string *error) const;
  bool discardChanges(std::string *error);
  // Discards tracked changes *and* deletes untracked files/directories, so the
  // working tree ends up exactly at HEAD.
  bool discardAllChanges(std::string *error);

  bool lastCommit(CommitInfo *out, std::string *error) const;
  std::vector<CommitInfo> log(std::size_t maxCount, std::string *error) const;
  // Walks every ref (plus HEAD), so the history is not limited to one branch.
  // `reachableFromHead` tells whether the commit is in HEAD's history.
  std::vector<CommitInfo> logAll(std::size_t maxCount, std::string *error) const;
  std::vector<std::string> branches(std::string *error) const;
  std::vector<std::string> remoteBranches(std::string *error) const;

  bool checkoutBranch(const std::string &branch, std::string *error);
  // Checks out "origin/x": reuses or creates the local branch "x" tracking it.
  bool checkoutRemoteBranch(const std::string &remoteBranch, std::string *error);
  bool checkoutCommit(const std::string &oid, std::string *error);
  bool commitAll(const std::string &message, std::string *error);
  bool undoLastCommit(std::string *error);

  AheadBehind aheadBehind(const std::string &branch, std::string *error) const;
  // Commits that exist on the remote branch but not in the local branch (the
  // "behind" side of ahead/behind).
  std::vector<CommitInfo> incomingCommits(const std::string &branch, std::size_t maxCount,
                                          std::string *error) const;
  std::time_t lastFetchTime() const;

  bool fetch(const Credentials &creds, std::string *error);
  PullOutcome pull(const Credentials &creds, std::string *error);
  bool push(const std::string &branch, const Credentials &creds, std::string *error);

  bool mergeBranch(const std::string &branch, std::string *error);

  bool stashSave(const std::string &message, std::string *error);
  std::vector<StashInfo> stashes(std::string *error) const;
  bool stashPop(std::size_t index, std::string *error);

private:
  std::string remoteName(std::string *error) const;
  git_repository *repo_ = nullptr;
};

} // namespace git
