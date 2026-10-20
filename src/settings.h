// Persistent settings: remotes, repositories, last selection.
//
// Stored as JSON in REAPER's resource path (reaper_git.json). Passwords and
// SSH key passphrases are obfuscated (see util::obfuscate) but not encrypted.
#pragma once

#include <string>
#include <vector>

namespace settings
{

enum class AuthMethod
{
  Password,
  SshKey,
};

struct Remote
{
  std::string name;
  std::string url;
  std::string username;
  AuthMethod auth = AuthMethod::Password;
  std::string secret;   // password, or path to the SSH private key
  std::string passphrase; // optional SSH key passphrase
};

struct Repository
{
  std::string name;
  std::string path;
};

class Store
{
public:
  static Store &get();

  bool load();
  bool save() const;

  std::string filePath() const;

  std::vector<Remote> &remotes() { return remotes_; }
  const std::vector<Remote> &remotes() const { return remotes_; }

  std::vector<Repository> &repositories() { return repositories_; }
  const std::vector<Repository> &repositories() const { return repositories_; }

  // Path of the repository the Project/History sections work on.
  std::string &currentRepository() { return currentRepository_; }
  const std::string &currentRepository() const { return currentRepository_; }

  const Repository *findRepository(const std::string &path) const;
  const Remote *findRemoteForUrl(const std::string &url) const;
  Remote *findRemote(const std::string &name);
  bool removeRemote(const std::string &name);
  bool removeRepository(const std::string &path);

  // Loads the JSON file once, on first use.
  void ensureLoaded();

private:
  Store() = default;

  std::vector<Remote> remotes_;
  std::vector<Repository> repositories_;
  std::string currentRepository_;
  bool loaded_ = false;
};

} // namespace settings
