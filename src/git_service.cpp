#include "git_service.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <sys/stat.h>

#include <git2.h>

#include "util.h"

namespace git
{

namespace
{

std::string lastErrorText()
{
  const git_error *err = git_error_last();
  if (!err || !err->message) return {};

  std::string message = err->message;

  // libgit2 probes the default config (system/xdg/global/ProgramData) during
  // remote operations and leaves that lookup's error behind even when the
  // operation failed for another reason. Reporting it would be misleading, so
  // drop it and let the caller describe what it was doing.
  if (message.find("ProgramData") != std::string::npos ||
      message.find("doesn't exist") != std::string::npos ||
      message.find("could not find") != std::string::npos)
    return {};

  return message;
}

void setError(std::string *error, const std::string &message)
{
  if (error) *error = message;
}

void setLastError(std::string *error, const char *context)
{
  const std::string detail = lastErrorText();
  setError(error, detail.empty() ? std::string(context) : std::string(context) + ": " + detail);
}

std::string oidToString(const git_oid *oid, std::size_t maxChars)
{
  if (!oid) return {};
  char buf[GIT_OID_MAX_HEXSIZE + 1] = {0};
  git_oid_tostr(buf, sizeof(buf), oid);
  std::string out(buf);
  if (maxChars && out.size() > maxChars) out.resize(maxChars);
  return out;
}

// Branch name out of a stash message: "WIP on <branch>: <oid> <subject>".
std::string stashBranchFromMessage(const std::string &message)
{
  const std::string prefix = "WIP on ";
  if (message.compare(0, prefix.size(), prefix) != 0) return {};
  const std::size_t colon = message.find(':', prefix.size());
  if (colon == std::string::npos) return {};
  return message.substr(prefix.size(), colon - prefix.size());
}

struct PushStatus
{
  bool rejected = false;
  std::string message;
};

// libgit2 hands one payload to every callback of a remote operation.
struct CallbackPayload
{
  const Credentials *creds = nullptr;
  PushStatus *push = nullptr;
};

int credentialsCallback(git_credential **out, const char * /*url*/, const char *usernameFromUrl,
                        unsigned int allowedTypes, void *payload)
{
  auto *ctx = static_cast<CallbackPayload *>(payload);
  const Credentials *creds = ctx ? ctx->creds : nullptr;
  const std::string username = (creds && !creds->username.empty())
                                   ? creds->username
                                   : (usernameFromUrl ? usernameFromUrl : "");

  const bool haveKey = creds && !creds->sshKeyPath.empty();

  // Use the method the user configured, not the first one the server offers:
  // an SSH server can advertise publickey *and* password. Answering with an
  // SSH key when the stored credential is a password aborted the connection
  // and surfaced a misleading libgit2 error.
  if (haveKey && (allowedTypes & GIT_CREDENTIAL_SSH_KEY))
  {
    const char *passphrase = creds->passphrase.empty() ? nullptr : creds->passphrase.c_str();
    return git_credential_ssh_key_new(out, username.c_str(), nullptr,
                                      creds->sshKeyPath.c_str(), passphrase);
  }

  if (!haveKey && (allowedTypes & GIT_CREDENTIAL_USERPASS_PLAINTEXT))
  {
    const char *password = creds ? creds->password.c_str() : "";
    return git_credential_userpass_plaintext_new(out, username.c_str(), password);
  }

  if (allowedTypes & GIT_CREDENTIAL_USERNAME)
    return git_credential_username_new(out, username.c_str());

  if (allowedTypes & GIT_CREDENTIAL_DEFAULT) return git_credential_default_new(out);

  return GIT_EUSER;
}

int pushUpdateCallback(const char *refname, const char *status, void *payload)
{
  auto *ctx = static_cast<CallbackPayload *>(payload);
  if (ctx && ctx->push && status && *status)
  {
    ctx->push->rejected = true;
    ctx->push->message = std::string(refname ? refname : "ref") + ": " + status;
  }
  return 0;
}

void setupCallbacks(git_remote_callbacks &callbacks, CallbackPayload &payload,
                    PushStatus *pushStatus)
{
  git_remote_init_callbacks(&callbacks, GIT_REMOTE_CALLBACKS_VERSION);
  callbacks.credentials = credentialsCallback;
  callbacks.payload = &payload;
  if (pushStatus)
  {
    callbacks.push_update_reference = pushUpdateCallback;
    payload.push = pushStatus;
  }
}

bool indexHasConflicts(git_repository *repo)
{
  git_index *index = nullptr;
  if (git_repository_index(&index, repo) != 0) return true;
  const bool conflicts = git_index_has_conflicts(index) != 0;
  git_index_free(index);
  return conflicts;
}

struct StashPayload
{
  std::vector<StashInfo> *out = nullptr;
  git_repository *repo = nullptr;
};

int stashCallback(std::size_t index, const char *message, const git_oid *oid, void *payload)
{
  auto *ctx = static_cast<StashPayload *>(payload);
  StashInfo info;
  info.index = index;
  info.oid = oidToString(oid, 8);
  info.summary = message ? message : "";
  info.branch = stashBranchFromMessage(info.summary);

  git_commit *commit = nullptr;
  if (git_commit_lookup(&commit, ctx->repo, oid) == 0)
  {
    const git_signature *author = git_commit_author(commit);
    if (author)
    {
      info.author = author->name ? author->name : "";
      info.when = author->when.time;
    }
    git_commit_free(commit);
  }
  if (ctx->out) ctx->out->push_back(std::move(info));
  return 0;
}

} // namespace

void init() { git_libgit2_init(); }

void shutdown() { git_libgit2_shutdown(); }

bool isRepository(const std::string &path)
{
  git_repository *repo = nullptr;
  const int rc = git_repository_open(&repo, util::normalizeSlashes(path).c_str());
  if (rc != 0) return false;
  git_repository_free(repo);
  return true;
}

bool initRepository(const std::string &path, std::string *error)
{
  git_repository *repo = nullptr;
  if (git_repository_init(&repo, util::normalizeSlashes(path).c_str(), 0) != 0)
  {
    setLastError(error, "could not create the repository");
    return false;
  }
  git_repository_free(repo);
  return true;
}

bool clone(const std::string &url, const std::string &destPath, const Credentials &creds,
           std::string *error)
{
  CallbackPayload payload{&creds, nullptr};
  git_remote_callbacks callbacks;
  setupCallbacks(callbacks, payload, nullptr);

  git_clone_options options = GIT_CLONE_OPTIONS_INIT;
  options.fetch_opts.callbacks = callbacks;

  git_repository *repo = nullptr;
  if (git_clone(&repo, url.c_str(), destPath.c_str(), &options) != 0)
  {
    setLastError(error, "clone failed");
    return false;
  }
  git_repository_free(repo);
  return true;
}

bool checkRemote(const std::string &url, const Credentials &creds, std::string *error)
{
  git_remote *remote = nullptr;
  if (git_remote_create_detached(&remote, url.c_str()) != 0)
  {
    setLastError(error, "invalid remote URL");
    return false;
  }

  CallbackPayload payload{&creds, nullptr};
  git_remote_callbacks callbacks;
  setupCallbacks(callbacks, payload, nullptr);

  const int connectRc =
      git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
  if (connectRc != 0)
  {
    if (connectRc == GIT_EUSER)
      setError(error,
               "the server did not offer an authentication method matching the stored "
               "credentials");
    else
      setLastError(error, "could not authenticate with the git server");
    git_remote_free(remote);
    return false;
  }

  const git_remote_head **heads = nullptr;
  std::size_t count = 0;
  const int ls = git_remote_ls(&heads, &count, remote);
  git_remote_disconnect(remote);
  git_remote_free(remote);
  if (ls != 0)
  {
    setLastError(error, "could not read the remote repository");
    return false;
  }
  return true;
}

bool checkAuthentication(const std::string &url, const Credentials &creds, std::string *error)
{
  git_remote *remote = nullptr;
  if (git_remote_create_detached(&remote, url.c_str()) != 0)
  {
    setLastError(error, "invalid server URL");
    return false;
  }

  CallbackPayload payload{&creds, nullptr};
  git_remote_callbacks callbacks;
  setupCallbacks(callbacks, payload, nullptr);

  // Connecting performs the whole SSH handshake/authentication; no repository
  // (and therefore no upload-pack) is needed for this check.
  const int rc = git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
  git_remote_disconnect(remote);
  git_remote_free(remote);

  if (rc == 0) return true;

  if (rc == GIT_EUSER)
  {
    setError(error,
             "the server did not offer an authentication method matching the stored credentials");
    return false;
  }

  const std::string detail = lastErrorText();

  if (rc == GIT_EAUTH)
  {
    setError(error, detail.empty() ? "the server rejected the credentials"
                                   : "the server rejected the credentials: " + detail);
    return false;
  }

  // HTTP(S) has no server-level authentication: only an explicit 401/403 means
  // the credentials were rejected. A 404 from a host-only URL still shows the
  // server is reachable.
  const std::string lowered = util::toLower(url);
  if (util::startsWith(lowered, "http"))
  {
    if (detail.find("401") != std::string::npos || detail.find("403") != std::string::npos)
    {
      setError(error, "the server rejected the credentials: " + detail);
      return false;
    }
    return true;
  }

  // SSH: authentication happens before the remote command. For a server URL
  // without a repository the server then fails the (expected) upload-pack
  // request, e.g. "fatal: '/' does not appear to be a git repository" - the
  // credentials were accepted. Only connection/authentication failures count.
  auto contains = [&detail](const char *needle) {
    return detail.find(needle) != std::string::npos;
  };
  const bool connectionProblem = contains("connect") || contains("Connect") ||
                                 contains("refused") || contains("resolve") ||
                                 contains("timed out") || contains("timeout") ||
                                 contains("unreachable") || contains("handshake") ||
                                 contains("authenticat") || contains("denied") ||
                                 contains("permission");
  if (connectionProblem)
  {
    setError(error, detail.empty() ? "could not connect to the server"
                                   : "could not connect to the server: " + detail);
    return false;
  }

  return true;
}

Repo::~Repo()
{
  if (repo_) git_repository_free(repo_);
}

Repo::Repo(Repo &&other) noexcept : repo_(other.repo_) { other.repo_ = nullptr; }

Repo &Repo::operator=(Repo &&other) noexcept
{
  if (this != &other)
  {
    if (repo_) git_repository_free(repo_);
    repo_ = other.repo_;
    other.repo_ = nullptr;
  }
  return *this;
}

bool Repo::open(const std::string &path, std::string *error)
{
  if (repo_)
  {
    git_repository_free(repo_);
    repo_ = nullptr;
  }
  if (git_repository_open(&repo_, util::normalizeSlashes(path).c_str()) != 0)
  {
    setLastError(error, "could not open repository");
    return false;
  }
  return true;
}

std::string Repo::workdir() const
{
  if (!repo_) return {};
  const char *dir = git_repository_workdir(repo_);
  return dir ? util::stripTrailingSep(dir) : std::string();
}

std::string Repo::remoteUrl(std::string *error) const
{
  if (!repo_)
  {
    setError(error, "no repository");
    return {};
  }
  const std::string name = remoteName(error);
  if (name.empty()) return {};
  git_remote *remote = nullptr;
  if (git_remote_lookup(&remote, repo_, name.c_str()) != 0)
  {
    setLastError(error, "could not read the remote URL");
    return {};
  }
  const char *url = git_remote_url(remote);
  const std::string out = url ? url : "";
  git_remote_free(remote);
  return out;
}

bool Repo::discardChanges(std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_FORCE;
  if (git_checkout_head(repo_, &options) != 0)
  {
    setLastError(error, "could not discard the current changes");
    return false;
  }
  return true;
}

bool Repo::discardAllChanges(std::string *error)
{
  if (!discardChanges(error)) return false;

  // Untracked files keep the tree dirty, so they are removed as well. This is
  // what makes "Clear Changes" leave the repository exactly at HEAD.
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

  git_status_list *list = nullptr;
  if (git_status_list_new(&list, repo_, &options) != 0)
  {
    setLastError(error, "could not read the working tree status");
    return false;
  }

  const std::string root = workdir();
  bool clean = true;
  std::vector<std::string> touchedDirs;

  const std::size_t count = git_status_list_entrycount(list);
  for (std::size_t i = 0; i < count; ++i)
  {
    const git_status_entry *entry = git_status_byindex(list, i);
    if (!entry) continue;

    // entry->status carries the GIT_STATUS_* flags; index_to_workdir->status
    // is a git_delta_t (GIT_DELTA_UNTRACKED), which is a different enum.
    const bool untracked = (entry->status & GIT_STATUS_WT_NEW) != 0;
    if (!untracked) continue;

    const char *relative =
        entry->index_to_workdir && entry->index_to_workdir->new_file.path
            ? entry->index_to_workdir->new_file.path
            : nullptr;
    if (!relative) continue;

    const std::string full = util::pathJoin(root, relative);
    touchedDirs.push_back(util::dirName(full));

    std::error_code ec;
    std::filesystem::remove_all(full, ec);
    if (ec) clean = false;
  }
  git_status_list_free(list);

  // Drop directories that only existed for the untracked files. Walking up from
  // each removed path avoids iterating a tree while it is being modified (that
  // threw "cannot increment recursive directory iterator").
  for (const std::string &dir : touchedDirs)
  {
    std::string current = util::stripTrailingSep(dir);
    while (!current.empty() && current.size() > root.size() &&
           util::startsWith(current, root))
    {
      std::error_code emptyError;
      if (!std::filesystem::is_empty(current, emptyError) || emptyError) break;

      std::error_code removeError;
      std::filesystem::remove(current, removeError);
      if (removeError) break;
      current = util::stripTrailingSep(util::dirName(current));
    }
  }

  if (!clean) setError(error, "some untracked files could not be removed");
  return clean;
}

std::string Repo::currentBranch(std::string *error) const
{
  if (!repo_)
  {
    setError(error, "no repository");
    return {};
  }
  git_reference *head = nullptr;
  const int headResult = git_repository_head(&head, repo_);
  if (headResult != 0)
  {
    if (headResult == GIT_EUNBORNBRANCH)
    {
      // HEAD points at a branch that has no commits yet: report its name.
      git_reference *symbolic = nullptr;
      std::string out;
      if (git_reference_lookup(&symbolic, repo_, "HEAD") == 0)
      {
        const char *target = git_reference_symbolic_target(symbolic);
        out = target ? target : "";
        const std::string prefix = "refs/heads/";
        if (out.compare(0, prefix.size(), prefix) == 0) out = out.substr(prefix.size());
        git_reference_free(symbolic);
      }
      return out;
    }
    setLastError(error, "could not read HEAD");
    return {};
  }
  const char *name = nullptr;
  std::string out;
  if (git_branch_name(&name, head) == 0 && name) out = name;
  else if (git_reference_is_branch(head)) out = git_reference_shorthand(head);
  git_reference_free(head);
  return out;
}

bool Repo::isHeadDetached() const
{
  return repo_ && git_repository_head_detached(repo_) == 1;
}

bool Repo::isHeadUnborn() const
{
  return repo_ && git_repository_head_unborn(repo_) == 1;
}

bool Repo::hasChanges(std::string *error) const
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                  GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

  git_status_list *list = nullptr;
  if (git_status_list_new(&list, repo_, &options) != 0)
  {
    setLastError(error, "could not read repository status");
    return false;
  }
  const bool dirty = git_status_list_entrycount(list) > 0;
  git_status_list_free(list);
  return dirty;
}

bool Repo::lastCommit(CommitInfo *out, std::string *error) const
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  git_reference *head = nullptr;
  if (git_repository_head(&head, repo_) != 0)
  {
    setError(error, "no commits yet");
    return false;
  }
  git_reference_free(head);

  git_object *obj = nullptr;
  if (git_revparse_single(&obj, repo_, "HEAD") != 0)
  {
    setLastError(error, "could not read HEAD");
    return false;
  }
  git_object *peeled = nullptr;
  if (git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT) != 0)
  {
    git_object_free(obj);
    setLastError(error, "could not read the HEAD commit");
    return false;
  }
  git_commit *commit = reinterpret_cast<git_commit *>(peeled);

  if (out)
  {
    out->fullOid = oidToString(git_commit_id(commit), 0);
    out->oid = oidToString(git_commit_id(commit), 8);
    out->summary = git_commit_summary(commit) ? git_commit_summary(commit) : "";
    out->parentCount = static_cast<int>(git_commit_parentcount(commit));
    const git_signature *author = git_commit_author(commit);
    if (author)
    {
      out->author = author->name ? author->name : "";
      out->when = author->when.time;
    }
  }

  git_object_free(peeled);
  git_object_free(obj);
  return true;
}

std::vector<CommitInfo> Repo::log(std::size_t maxCount, std::string *error) const
{
  std::vector<CommitInfo> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }

  git_revwalk *walk = nullptr;
  if (git_revwalk_new(&walk, repo_) != 0)
  {
    setLastError(error, "could not walk the history");
    return out;
  }
  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  if (git_revwalk_push_head(walk) != 0)
  {
    git_revwalk_free(walk);
    setLastError(error, "could not read HEAD");
    return out;
  }

  git_oid oid;
  while (out.size() < maxCount && git_revwalk_next(&oid, walk) == 0)
  {
    git_commit *commit = nullptr;
    if (git_commit_lookup(&commit, repo_, &oid) != 0) continue;

    CommitInfo info;
    info.fullOid = oidToString(&oid, 0);
    info.oid = oidToString(&oid, 8);
    info.summary = git_commit_summary(commit) ? git_commit_summary(commit) : "";
    info.parentCount = static_cast<int>(git_commit_parentcount(commit));
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

std::vector<std::string> Repo::branches(std::string *error) const
{
  std::vector<std::string> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }
  git_branch_iterator *it = nullptr;
  if (git_branch_iterator_new(&it, repo_, GIT_BRANCH_LOCAL) != 0)
  {
    setLastError(error, "could not list branches");
    return out;
  }
  git_reference *ref = nullptr;
  git_branch_t type;
  while (git_branch_next(&ref, &type, it) == 0)
  {
    const char *name = nullptr;
    if (git_branch_name(&name, ref) == 0 && name) out.emplace_back(name);
    git_reference_free(ref);
    ref = nullptr;
  }
  git_branch_iterator_free(it);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> Repo::remoteBranches(std::string *error) const
{
  std::vector<std::string> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }
  git_branch_iterator *it = nullptr;
  if (git_branch_iterator_new(&it, repo_, GIT_BRANCH_REMOTE) != 0)
  {
    setLastError(error, "could not list remote branches");
    return out;
  }
  git_reference *ref = nullptr;
  git_branch_t type;
  while (git_branch_next(&ref, &type, it) == 0)
  {
    const char *name = nullptr;
    if (git_branch_name(&name, ref) == 0 && name)
    {
      const std::string branch = name;
      // refs/remotes/<remote>/HEAD is a symbolic alias, not a branch.
      if (!util::endsWith(branch, "/HEAD")) out.push_back(branch);
    }
    git_reference_free(ref);
    ref = nullptr;
  }
  git_branch_iterator_free(it);
  std::sort(out.begin(), out.end());
  return out;
}

bool Repo::checkoutRemoteBranch(const std::string &remoteBranch, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  const std::size_t slash = remoteBranch.find('/');
  if (slash == std::string::npos || slash + 1 >= remoteBranch.size())
  {
    setError(error, "invalid remote branch " + remoteBranch);
    return false;
  }
  const std::string localName = remoteBranch.substr(slash + 1);

  // Reuse an existing local branch of that name, otherwise create it at the
  // remote branch and make it track that branch (git checkout --track).
  git_reference *localRef = nullptr;
  if (git_branch_lookup(&localRef, repo_, localName.c_str(), GIT_BRANCH_LOCAL) == 0)
  {
    git_reference_free(localRef);
    return checkoutBranch(localName, error);
  }

  git_reference *remoteRef = nullptr;
  if (git_branch_lookup(&remoteRef, repo_, remoteBranch.c_str(), GIT_BRANCH_REMOTE) != 0)
  {
    setError(error, "unknown remote branch " + remoteBranch);
    return false;
  }

  git_object *peeled = nullptr;
  if (git_reference_peel(&peeled, remoteRef, GIT_OBJECT_COMMIT) != 0)
  {
    git_reference_free(remoteRef);
    setError(error, "could not read " + remoteBranch);
    return false;
  }

  git_reference *created = nullptr;
  const int createdRc = git_branch_create(&created, repo_, localName.c_str(),
                                          reinterpret_cast<const git_commit *>(peeled), 0);
  git_object_free(peeled);
  git_reference_free(remoteRef);
  if (createdRc != 0)
  {
    setError(error, "could not create local branch " + localName);
    return false;
  }

  git_branch_set_upstream(created, remoteBranch.c_str());
  git_reference_free(created);

  return checkoutBranch(localName, error);
}

bool Repo::checkoutBranch(const std::string &branch, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  const std::string refname = "refs/heads/" + branch;
  git_object *target = nullptr;
  if (git_revparse_single(&target, repo_, refname.c_str()) != 0)
  {
    setError(error, "unknown branch " + branch);
    return false;
  }

  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_SAFE;
  if (git_checkout_tree(repo_, target, &options) != 0)
  {
    git_object_free(target);
    setError(error, "could not check out " + branch);
    return false;
  }
  git_object_free(target);

  if (git_repository_set_head(repo_, refname.c_str()) != 0)
  {
    setError(error, "could not switch HEAD to " + branch);
    return false;
  }
  return true;
}

bool Repo::checkoutCommit(const std::string &oid, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  git_object *target = nullptr;
  if (git_revparse_single(&target, repo_, oid.c_str()) != 0)
  {
    setError(error, "unknown commit " + oid);
    return false;
  }

  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_SAFE;
  if (git_checkout_tree(repo_, target, &options) != 0)
  {
    git_object_free(target);
    setError(error, "could not check out commit " + oid);
    return false;
  }

  const int rc = git_repository_set_head_detached(repo_, git_object_id(target));
  git_object_free(target);
  if (rc != 0)
  {
    setLastError(error, "could not detach HEAD");
    return false;
  }
  return true;
}

bool Repo::commitAll(const std::string &message, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  git_index *index = nullptr;
  if (git_repository_index(&index, repo_) != 0)
  {
    setLastError(error, "could not open the index");
    return false;
  }

  if (git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) != 0 ||
      git_index_write(index) != 0)
  {
    git_index_free(index);
    setLastError(error, "could not stage changes");
    return false;
  }

  git_oid treeOid;
  if (git_index_write_tree(&treeOid, index) != 0)
  {
    git_index_free(index);
    setLastError(error, "could not write the index tree");
    return false;
  }
  git_index_free(index);

  git_tree *tree = nullptr;
  if (git_tree_lookup(&tree, repo_, &treeOid) != 0)
  {
    setLastError(error, "could not read the index tree");
    return false;
  }

  git_reference *head = nullptr;
  git_commit *parent = nullptr;
  const git_commit *parents[1] = {nullptr};
  int parentCount = 0;
  if (git_repository_head(&head, repo_) == 0)
  {
    git_oid headOid;
    if (git_reference_name_to_id(&headOid, repo_, "HEAD") == 0 &&
        git_commit_lookup(&parent, repo_, &headOid) == 0)
    {
      parents[0] = parent;
      parentCount = 1;
    }
  }

  git_signature *signature = nullptr;
  if (git_signature_default(&signature, repo_) != 0)
  {
    if (git_signature_now(&signature, "reaper_git", "reaper_git@localhost") != 0)
    {
      git_tree_free(tree);
      if (parent) git_commit_free(parent);
      if (head) git_reference_free(head);
      setLastError(error, "could not create a commit signature");
      return false;
    }
  }

  git_oid commitOid;
  const int rc = git_commit_create(&commitOid, repo_, "HEAD", signature, signature, nullptr,
                                   message.c_str(), tree, parentCount, parents);

  git_signature_free(signature);
  git_tree_free(tree);
  if (parent) git_commit_free(parent);
  if (head) git_reference_free(head);

  if (rc != 0)
  {
    setLastError(error, "could not create the commit");
    return false;
  }
  return true;
}

bool Repo::undoLastCommit(std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }
  git_reference *head = nullptr;
  if (git_repository_head(&head, repo_) != 0)
  {
    setLastError(error, "could not read HEAD");
    return false;
  }
  git_commit *commit = nullptr;
  if (git_commit_lookup(&commit, repo_, git_reference_target(head)) != 0)
  {
    git_reference_free(head);
    setLastError(error, "could not read the last commit");
    return false;
  }
  if (git_commit_parentcount(commit) == 0)
  {
    git_commit_free(commit);
    git_reference_free(head);
    setError(error, "the last commit has no parent, it cannot be undone");
    return false;
  }
  git_commit *parent = nullptr;
  if (git_commit_parent(&parent, commit, 0) != 0)
  {
    git_commit_free(commit);
    git_reference_free(head);
    setLastError(error, "could not read the parent commit");
    return false;
  }

  const int rc = git_reset(repo_, (git_object *)parent, GIT_RESET_SOFT, nullptr);
  git_commit_free(parent);
  git_commit_free(commit);
  git_reference_free(head);
  if (rc != 0)
  {
    setLastError(error, "could not undo the commit");
    return false;
  }
  return true;
}

AheadBehind Repo::aheadBehind(const std::string &branch, std::string *error) const
{
  AheadBehind out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }

  git_reference *branchRef = nullptr;
  if (git_branch_lookup(&branchRef, repo_, branch.c_str(), GIT_BRANCH_LOCAL) != 0)
  {
    setError(error, "unknown branch " + branch);
    return out;
  }

  git_reference *upstream = nullptr;
  if (git_branch_upstream(&upstream, branchRef) != 0)
  {
    git_reference_free(branchRef);
    return out; // no upstream configured
  }

  const git_oid *localOid = git_reference_target(branchRef);
  const git_oid *upstreamOid = git_reference_target(upstream);
  if (localOid && upstreamOid)
  {
    std::size_t ahead = 0;
    std::size_t behind = 0;
    if (git_graph_ahead_behind(&ahead, &behind, repo_, localOid, upstreamOid) == 0)
    {
      out.ahead = static_cast<int>(ahead);
      out.behind = static_cast<int>(behind);
      out.hasUpstream = true;
    }
  }

  git_reference_free(upstream);
  git_reference_free(branchRef);
  return out;
}

std::vector<CommitInfo> Repo::incomingCommits(const std::string &branch, std::size_t maxCount,
                                             std::string *error) const
{
  std::vector<CommitInfo> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }

  git_reference *branchRef = nullptr;
  if (git_branch_lookup(&branchRef, repo_, branch.c_str(), GIT_BRANCH_LOCAL) != 0)
  {
    setError(error, "unknown branch " + branch);
    return out;
  }

  git_reference *upstream = nullptr;
  if (git_branch_upstream(&upstream, branchRef) != 0)
  {
    git_reference_free(branchRef);
    return out; // nothing to compare against
  }

  git_revwalk *walk = nullptr;
  if (git_revwalk_new(&walk, repo_) != 0)
  {
    git_reference_free(upstream);
    git_reference_free(branchRef);
    setLastError(error, "could not walk the history");
    return out;
  }

  git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  const int pushed = git_revwalk_push_ref(walk, git_reference_name(upstream));
  const int hidden = git_revwalk_hide_ref(walk, git_reference_name(branchRef));
  git_reference_free(upstream);
  git_reference_free(branchRef);

  if (pushed != 0 || hidden != 0)
  {
    git_revwalk_free(walk);
    setLastError(error, "could not compare with the upstream branch");
    return out;
  }

  git_oid oid;
  while (out.size() < maxCount && git_revwalk_next(&oid, walk) == 0)
  {
    git_commit *commit = nullptr;
    if (git_commit_lookup(&commit, repo_, &oid) != 0) continue;

    CommitInfo info;
    info.fullOid = oidToString(&oid, 0);
    info.oid = oidToString(&oid, 8);
    const char *summary = git_commit_summary(commit);
    info.summary = summary ? summary : "";
    info.parentCount = static_cast<int>(git_commit_parentcount(commit));
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

std::time_t Repo::lastFetchTime() const
{
  if (!repo_) return 0;
  const std::string fetchHead = std::string(git_repository_path(repo_)) + "FETCH_HEAD";
  struct stat st {};
  if (::stat(fetchHead.c_str(), &st) != 0) return 0;
  return st.st_mtime;
}

std::string Repo::remoteName(std::string *error) const
{
  if (!repo_)
  {
    setError(error, "no repository");
    return {};
  }
  git_strarray names{};
  if (git_remote_list(&names, repo_) != 0)
  {
    setLastError(error, "could not list remotes");
    return {};
  }
  std::string chosen;
  for (std::size_t i = 0; i < names.count; ++i)
  {
    const std::string name = names.strings[i];
    if (name == "origin")
    {
      chosen = name;
      break;
    }
    if (chosen.empty()) chosen = name;
  }
  git_strarray_dispose(&names);
  if (chosen.empty()) setError(error, "the repository has no remote");
  return chosen;
}

bool Repo::fetch(const Credentials &creds, std::string *error)
{
  const std::string name = remoteName(error);
  if (name.empty()) return false;

  git_remote *remote = nullptr;
  if (git_remote_lookup(&remote, repo_, name.c_str()) != 0)
  {
    setLastError(error, "could not open the remote");
    return false;
  }

  // Older/partially configured clones may have no fetch refspec; without it a
  // fetch does not create refs/remotes/<remote>/* and git then reports "the
  // upstream is gone".
  git_strarray specs{};
  if (git_remote_get_fetch_refspecs(&specs, remote) == 0 && specs.count == 0)
    git_remote_add_fetch(repo_, name.c_str(), ("+refs/heads/*:refs/remotes/" + name + "/*").c_str());
  git_strarray_dispose(&specs);

  CallbackPayload payload{&creds, nullptr};
  git_remote_callbacks callbacks;
  setupCallbacks(callbacks, payload, nullptr);

  git_fetch_options options = GIT_FETCH_OPTIONS_INIT;
  options.callbacks = callbacks;

  const int rc = git_remote_fetch(remote, nullptr, &options, nullptr);
  git_remote_free(remote);
  if (rc != 0)
  {
    setLastError(error, "fetch failed");
    return false;
  }

  // Make sure the current branch tracks the fetched remote branch, so git
  // status/pull behave consistently.
  git_reference *head = nullptr;
  if (git_repository_head(&head, repo_) == 0)
  {
    const char *branchName = nullptr;
    if (git_branch_name(&branchName, head) == 0 && branchName)
    {
      const std::string branch = branchName;
      git_reference *tracking = nullptr;
      const std::string trackingName = "refs/remotes/" + name + "/" + branch;
      if (git_reference_lookup(&tracking, repo_, trackingName.c_str()) == 0)
      {
        git_reference_free(tracking);
        git_reference *upstream = nullptr;
        if (git_branch_upstream(&upstream, head) == 0)
        {
          git_reference_free(upstream);
        }
        else
        {
          git_config *config = nullptr;
          if (git_repository_config(&config, repo_) == 0)
          {
            const std::string remoteKey = "branch." + branch + ".remote";
            const std::string mergeKey = "branch." + branch + ".merge";
            git_config_set_string(config, remoteKey.c_str(), name.c_str());
            git_config_set_string(config, mergeKey.c_str(), ("refs/heads/" + branch).c_str());
            git_config_free(config);
          }
        }
      }
    }
    git_reference_free(head);
  }

  return true;
}

PullOutcome Repo::pull(const Credentials &creds, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return PullOutcome::Failed;
  }
  if (!fetch(creds, error)) return PullOutcome::Failed;

  const std::string branch = currentBranch(error);
  if (branch.empty()) return PullOutcome::Failed;

  git_reference *branchRef = nullptr;
  if (git_branch_lookup(&branchRef, repo_, branch.c_str(), GIT_BRANCH_LOCAL) != 0)
  {
    setLastError(error, "could not read the current branch");
    return PullOutcome::Failed;
  }

  git_annotated_commit *annotated = nullptr;
  git_reference *upstream = nullptr;
  bool haveTarget = false;

  if (git_branch_upstream(&upstream, branchRef) == 0)
  {
    git_object *target = nullptr;
    if (git_reference_peel(&target, upstream, GIT_OBJECT_COMMIT) == 0)
    {
      haveTarget = git_annotated_commit_lookup(&annotated, repo_, git_object_id(target)) == 0;
      git_object_free(target);
    }
    git_reference_free(upstream);
  }

  if (!haveTarget)
  {
    git_object *fetchHead = nullptr;
    if (git_revparse_single(&fetchHead, repo_, "FETCH_HEAD") == 0)
    {
      haveTarget = git_annotated_commit_lookup(&annotated, repo_, git_object_id(fetchHead)) == 0;
      git_object_free(fetchHead);
    }
  }

  if (!haveTarget)
  {
    git_reference_free(branchRef);
    setError(error, "there is nothing to pull from (no upstream branch)");
    return PullOutcome::Failed;
  }

  git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
  git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
  const git_annotated_commit *heads[1] = {annotated};
  if (git_merge_analysis(&analysis, &preference, repo_, heads, 1) != 0)
  {
    git_annotated_commit_free(annotated);
    git_reference_free(branchRef);
    setLastError(error, "could not analyse the incoming changes");
    return PullOutcome::Failed;
  }

  if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)
  {
    git_annotated_commit_free(annotated);
    git_reference_free(branchRef);
    return PullOutcome::UpToDate;
  }

  git_checkout_options checkoutOptions = GIT_CHECKOUT_OPTIONS_INIT;
  checkoutOptions.checkout_strategy = GIT_CHECKOUT_SAFE;

  if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD)
  {
    git_object *target = nullptr;
    bool ok = false;
    if (git_object_lookup(&target, repo_, git_annotated_commit_id(annotated), GIT_OBJECT_COMMIT) ==
            0 &&
        git_checkout_tree(repo_, target, &checkoutOptions) == 0)
    {
      git_reference *updated = nullptr;
      ok = git_reference_set_target(&updated, branchRef, git_annotated_commit_id(annotated),
                                    "pull: fast-forward") == 0;
      if (updated) git_reference_free(updated);
    }
    if (target) git_object_free(target);
    git_annotated_commit_free(annotated);
    git_reference_free(branchRef);
    if (!ok)
    {
      setLastError(error, "could not fast-forward the working tree");
      return PullOutcome::Failed;
    }
    return PullOutcome::FastForwarded;
  }

  git_merge_options mergeOptions = GIT_MERGE_OPTIONS_INIT;
  if (git_merge(repo_, heads, 1, &mergeOptions, &checkoutOptions) != 0)
  {
    git_annotated_commit_free(annotated);
    git_reference_free(branchRef);
    setLastError(error, "merge failed");
    return PullOutcome::Failed;
  }

  if (indexHasConflicts(repo_))
  {
    git_repository_state_cleanup(repo_);
    git_annotated_commit_free(annotated);
    git_reference_free(branchRef);
    setError(error, "the merge has conflicts; resolve them manually");
    return PullOutcome::Failed;
  }

  // Create the merge commit: index tree, parents = HEAD + fetched commit.
  git_index *index = nullptr;
  git_oid treeOid;
  git_tree *tree = nullptr;
  git_signature *signature = nullptr;
  git_commit *headCommit = nullptr;
  git_commit *theirCommit = nullptr;
  bool merged = false;

  if (git_repository_index(&index, repo_) == 0 && git_index_write_tree(&treeOid, index) == 0 &&
      git_tree_lookup(&tree, repo_, &treeOid) == 0)
  {
    const git_commit *parents[2] = {nullptr, nullptr};
    if (git_commit_lookup(&headCommit, repo_, git_reference_target(branchRef)) == 0)
      parents[0] = headCommit;
    if (git_commit_lookup(&theirCommit, repo_, git_annotated_commit_id(annotated)) == 0)
      parents[1] = theirCommit;

    if (git_signature_default(&signature, repo_) != 0)
      git_signature_now(&signature, "reaper_git", "reaper_git@localhost");

    const std::string message = "Merge remote-tracking branch into " + branch;
    git_oid commitOid;
    merged = parents[0] && parents[1] && signature &&
             git_commit_create(&commitOid, repo_, "HEAD", signature, signature, nullptr,
                               message.c_str(), tree, 2, parents) == 0;
  }

  if (index) git_index_free(index);
  if (tree) git_tree_free(tree);
  if (signature) git_signature_free(signature);
  if (headCommit) git_commit_free(headCommit);
  if (theirCommit) git_commit_free(theirCommit);
  git_annotated_commit_free(annotated);
  git_reference_free(branchRef);

  if (!merged)
  {
    setLastError(error, "could not create the merge commit");
    return PullOutcome::Failed;
  }
  return PullOutcome::Merged;
}

bool Repo::push(const std::string &branch, const Credentials &creds, std::string *error)
{
  const std::string name = remoteName(error);
  if (name.empty()) return false;

  git_remote *remote = nullptr;
  if (git_remote_lookup(&remote, repo_, name.c_str()) != 0)
  {
    setLastError(error, "could not open the remote");
    return false;
  }

  PushStatus pushStatus;
  CallbackPayload payload{&creds, &pushStatus};
  git_remote_callbacks callbacks;
  setupCallbacks(callbacks, payload, &pushStatus);

  git_push_options options = GIT_PUSH_OPTIONS_INIT;
  options.callbacks = callbacks;

  const std::string refspec = "refs/heads/" + branch + ":refs/heads/" + branch;
  char *refspecPtr = const_cast<char *>(refspec.c_str());
  git_strarray refspecs{&refspecPtr, 1};

  const int rc = git_remote_push(remote, &refspecs, &options);
  git_remote_free(remote);

  if (rc != 0)
  {
    setLastError(error, "push failed");
    return false;
  }
  if (pushStatus.rejected)
  {
    setError(error, pushStatus.message);
    return false;
  }

  // Confirm the server really has our commit, and make the branch track it.
  git_reference *branchRef = nullptr;
  git_oid localOid;
  bool haveLocal = false;
  if (git_branch_lookup(&branchRef, repo_, branch.c_str(), GIT_BRANCH_LOCAL) == 0 &&
      git_reference_target(branchRef))
  {
    git_oid_cpy(&localOid, git_reference_target(branchRef));
    haveLocal = true;

    git_reference *upstream = nullptr;
    if (git_branch_upstream(&upstream, branchRef) == 0)
    {
      git_reference_free(upstream);
    }
    else
    {
      git_config *config = nullptr;
      if (git_repository_config(&config, repo_) == 0)
      {
        const std::string remoteKey = "branch." + branch + ".remote";
        const std::string mergeKey = "branch." + branch + ".merge";
        git_config_set_string(config, remoteKey.c_str(), name.c_str());
        git_config_set_string(config, mergeKey.c_str(), ("refs/heads/" + branch).c_str());
        git_config_free(config);
      }
    }
  }
  if (branchRef) git_reference_free(branchRef);
  if (!haveLocal) return true;

  git_remote *verify = nullptr;
  if (git_remote_lookup(&verify, repo_, name.c_str()) != 0) return true;

  CallbackPayload verifyPayload{&creds, nullptr};
  git_remote_callbacks verifyCallbacks;
  setupCallbacks(verifyCallbacks, verifyPayload, nullptr);
  if (git_remote_connect(verify, GIT_DIRECTION_FETCH, &verifyCallbacks, nullptr, nullptr) != 0)
  {
    git_remote_free(verify);
    return true; // push succeeded; verification is best effort
  }

  const git_remote_head **heads = nullptr;
  std::size_t count = 0;
  bool found = false;
  bool matches = false;
  if (git_remote_ls(&heads, &count, verify) == 0)
  {
    const std::string wanted = "refs/heads/" + branch;
    for (std::size_t i = 0; i < count; ++i)
    {
      if (!heads[i] || !heads[i]->name || heads[i]->name != wanted) continue;
      found = true;
      matches = git_oid_cmp(&heads[i]->oid, &localOid) == 0;
      break;
    }
  }
  git_remote_disconnect(verify);
  git_remote_free(verify);

  if (!found)
  {
    setError(error, "the server does not have branch '" + branch + "' after the push");
    return false;
  }
  if (!matches)
  {
    setError(error, "the server's '" + branch + "' does not point at the pushed commit");
    return false;
  }
  return true;
}

bool Repo::mergeBranch(const std::string &branch, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }
  git_reference *branchRef = nullptr;
  if (git_branch_lookup(&branchRef, repo_, branch.c_str(), GIT_BRANCH_LOCAL) != 0)
  {
    setError(error, "unknown branch " + branch);
    return false;
  }

  git_annotated_commit *annotated = nullptr;
  const int lookup =
      git_annotated_commit_lookup(&annotated, repo_, git_reference_target(branchRef));
  git_reference_free(branchRef);
  if (lookup != 0)
  {
    setError(error, "could not read branch " + branch);
    return false;
  }

  git_merge_options mergeOptions = GIT_MERGE_OPTIONS_INIT;
  git_checkout_options checkoutOptions = GIT_CHECKOUT_OPTIONS_INIT;
  checkoutOptions.checkout_strategy = GIT_CHECKOUT_SAFE;

  git_oid theirOid;
  git_oid_cpy(&theirOid, git_annotated_commit_id(annotated));

  const git_annotated_commit *heads[1] = {annotated};
  const int rc = git_merge(repo_, heads, 1, &mergeOptions, &checkoutOptions);
  git_annotated_commit_free(annotated);

  if (rc != 0)
  {
    setLastError(error, "merge failed");
    return false;
  }
  if (indexHasConflicts(repo_))
  {
    git_repository_state_cleanup(repo_);
    setError(error, "the merge has conflicts; resolve them manually");
    return false;
  }

  // Turn the staged merge into a real merge commit (two parents).
  git_index *index = nullptr;
  git_oid treeOid;
  git_tree *tree = nullptr;
  git_signature *signature = nullptr;
  git_commit *headCommit = nullptr;
  git_commit *theirCommit = nullptr;
  bool committed = false;

  if (git_repository_index(&index, repo_) == 0 && git_index_write_tree(&treeOid, index) == 0 &&
      git_tree_lookup(&tree, repo_, &treeOid) == 0)
  {
    git_reference *head = nullptr;
    if (git_repository_head(&head, repo_) == 0)
    {
      const git_commit *parents[2] = {nullptr, nullptr};
      if (git_commit_lookup(&headCommit, repo_, git_reference_target(head)) == 0)
        parents[0] = headCommit;
      if (git_commit_lookup(&theirCommit, repo_, &theirOid) == 0) parents[1] = theirCommit;

      if (git_signature_default(&signature, repo_) != 0)
        git_signature_now(&signature, "reaper_git", "reaper_git@localhost");

      const std::string message = "Merge branch '" + branch + "'";
      git_oid commitOid;
      committed = parents[0] && parents[1] && signature &&
                  git_commit_create(&commitOid, repo_, "HEAD", signature, signature, nullptr,
                                    message.c_str(), tree, 2, parents) == 0;
      git_reference_free(head);
    }
  }

  if (index) git_index_free(index);
  if (tree) git_tree_free(tree);
  if (signature) git_signature_free(signature);
  if (headCommit) git_commit_free(headCommit);
  if (theirCommit) git_commit_free(theirCommit);

  if (!committed)
  {
    setLastError(error, "could not create the merge commit");
    return false;
  }
  return true;
}

bool Repo::stashSave(const std::string &message, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  // libgit2 requires an explicit stasher signature (NULL is "invalid argument").
  git_signature *signature = nullptr;
  if (git_signature_default(&signature, repo_) != 0 || !signature)
  {
    if (git_signature_now(&signature, "reaper_git", "reaper_git@localhost") != 0 || !signature)
    {
      setLastError(error, "could not create a signature for the stash");
      return false;
    }
  }

  git_oid oid;
  // Untracked files are part of a REAPER project as much as the .rpp itself;
  // leaving them behind would keep the tree dirty after the stash.
  const int rc = git_stash_save(&oid, repo_, signature, message.empty() ? nullptr : message.c_str(),
                                GIT_STASH_DEFAULT | GIT_STASH_INCLUDE_UNTRACKED);
  git_signature_free(signature);
  if (rc != 0)
  {
    setLastError(error, "could not stash the changes");
    return false;
  }
  return true;
}

std::vector<StashInfo> Repo::stashes(std::string *error) const
{
  std::vector<StashInfo> out;
  if (!repo_)
  {
    setError(error, "no repository");
    return out;
  }
  StashPayload payload{&out, repo_};
  if (git_stash_foreach(repo_, stashCallback, &payload) != 0)
  {
    setLastError(error, "could not read the stash list");
    out.clear();
  }
  return out;
}

bool Repo::stashPop(std::size_t index, std::string *error)
{
  if (!repo_)
  {
    setError(error, "no repository");
    return false;
  }

  // Recovering a stash must never leave the project in a half-merged state.
  // Instead of git_stash_pop (which merges the stash tree and can conflict,
  // badly so for REAPER projects that mix text and binary data) we restore the
  // stashed content of exactly the files the stash changed, preferring the
  // stashed version. Conflicts are therefore impossible by construction.
  struct Lookup
  {
    std::size_t index;
    git_oid oid;
    bool found;
  } lookup{index, {}, false};

  git_stash_foreach(
      repo_,
      [](std::size_t i, const char *, const git_oid *oid, void *payload) -> int {
        auto *state = static_cast<Lookup *>(payload);
        if (i != state->index) return 0;
        git_oid_cpy(&state->oid, oid);
        state->found = true;
        return 1; // stop
      },
      &lookup);
  if (!lookup.found)
  {
    setError(error, "the selected stash entry no longer exists");
    return false;
  }

  git_commit *stashCommit = nullptr;
  if (git_commit_lookup(&stashCommit, repo_, &lookup.oid) != 0)
  {
    setLastError(error, "could not read the stash");
    return false;
  }
  if (git_commit_parentcount(stashCommit) < 1)
  {
    git_commit_free(stashCommit);
    setError(error, "the stash entry has no base commit");
    return false;
  }

  git_commit *base = nullptr;
  git_commit_parent(&base, stashCommit, 0);
  git_tree *stashTree = nullptr;
  git_tree *baseTree = nullptr;
  git_diff *diff = nullptr;
  git_commit_tree(&stashTree, stashCommit);
  git_commit_tree(&baseTree, base);

  std::vector<std::string> paths;
  if (git_diff_tree_to_tree(&diff, repo_, baseTree, stashTree, nullptr) == 0)
  {
    const std::size_t count = git_diff_num_deltas(diff);
    for (std::size_t i = 0; i < count; ++i)
    {
      const git_diff_delta *delta = git_diff_get_delta(diff, i);
      if (!delta) continue;
      const char *path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
      if (path) paths.emplace_back(path);
    }
  }

  bool ok = true;
  if (!paths.empty() && stashTree)
  {
    std::vector<char *> pointers;
    pointers.reserve(paths.size());
    for (auto &path : paths) pointers.push_back(const_cast<char *>(path.c_str()));

    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    options.checkout_strategy = GIT_CHECKOUT_FORCE;
    options.paths = {pointers.data(), pointers.size()};
    ok = git_checkout_tree(repo_, reinterpret_cast<git_object *>(stashTree), &options) == 0;
    if (!ok) setLastError(error, "could not restore the stashed files");
  }

  if (ok && git_stash_drop(repo_, index) != 0)
  {
    ok = false;
    setLastError(error, "could not remove the stash entry");
  }

  if (!ok)
  {
    // Roll the tracked files back to HEAD: the operation is reversed.
    git_checkout_options reset = GIT_CHECKOUT_OPTIONS_INIT;
    reset.checkout_strategy = GIT_CHECKOUT_FORCE;
    git_checkout_head(repo_, &reset);
  }

  if (diff) git_diff_free(diff);
  if (baseTree) git_tree_free(baseTree);
  if (stashTree) git_tree_free(stashTree);
  if (base) git_commit_free(base);
  git_commit_free(stashCommit);

  return ok;
}

} // namespace git
