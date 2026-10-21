#include "settings.h"

#include <fstream>
#include <sstream>

#include "json.hpp"
#include "reaper_api.h"
#include "util.h"

namespace settings
{

namespace
{

std::string resourceFilePath()
{
  std::string dir;
  if (GetResourcePath) dir = GetResourcePath();
  if (dir.empty()) return "reaper_git.json";
  return util::pathJoin(dir, "reaper_git.json");
}

std::string authToString(AuthMethod m)
{
  return m == AuthMethod::SshKey ? "ssh" : "password";
}

AuthMethod authFromString(const std::string &s)
{
  return s == "ssh" ? AuthMethod::SshKey : AuthMethod::Password;
}

// Best-effort host extraction: "https://host/path", "git@host:path",
// "ssh://git@host/path" -> "host".
std::string hostOf(const std::string &url)
{
  std::string rest = url;
  const bool hasScheme = rest.find("://") != std::string::npos;
  const std::size_t scheme = rest.find("://");
  if (hasScheme) rest = rest.substr(scheme + 3);

  const std::size_t at = rest.find('@');
  if (at != std::string::npos) rest = rest.substr(at + 1);

  std::size_t end = rest.find('/');
  if (end != std::string::npos) rest = rest.substr(0, end);

  // With an explicit scheme the colon separates host and port, and the port is
  // part of the server identity. In the scp-like form (user@host:path) it
  // separates the host from the path instead.
  if (!hasScheme)
  {
    const std::size_t colon = rest.find(':');
    if (colon != std::string::npos) rest = rest.substr(0, colon);
  }

  return util::toLower(rest);
}

} // namespace

Store &Store::get()
{
  static Store instance;
  return instance;
}

std::string Store::filePath() const { return resourceFilePath(); }

void Store::ensureLoaded()
{
  if (!loaded_) load();
}

bool Store::load()
{
  loaded_ = true;
  remotes_.clear();
  repositories_.clear();
  hostKeys_.clear();
  currentRepository_.clear();

  const std::string path = filePath();
  std::ifstream in(path);
  if (!in)
    return true; // no settings yet - empty configuration

  nlohmann::json root;
  try
  {
    in >> root;
  }
  catch (const std::exception &)
  {
    return false; // corrupt file: keep running with empty settings
  }

  try
  {
    if (root.contains("remotes") && root["remotes"].is_array())
    {
      for (const auto &item : root["remotes"])
      {
        Remote r;
        r.name = item.value("name", std::string());
        r.url = item.value("url", std::string());
        r.username = item.value("username", std::string());
        r.auth = authFromString(item.value("auth", std::string("password")));
        r.secret = util::deobfuscate(item.value("secret", std::string()));
        r.passphrase = util::deobfuscate(item.value("passphrase", std::string()));
        if (!r.url.empty()) remotes_.push_back(std::move(r));
      }
    }

    if (root.contains("repositories") && root["repositories"].is_array())
    {
      for (const auto &item : root["repositories"])
      {
        Repository r;
        r.name = item.value("name", std::string());
        r.path = item.value("path", std::string());
        if (!r.path.empty()) repositories_.push_back(std::move(r));
      }
    }

    if (root.contains("host_keys") && root["host_keys"].is_object())
      for (const auto &item : root["host_keys"].items())
        if (item.value().is_string())
          hostKeys_[item.key()] = item.value().get<std::string>();

    currentRepository_ = root.value("current_repository", std::string());
  }
  catch (const std::exception &)
  {
    return false;
  }

  return true;
}

bool Store::save() const
{
  nlohmann::json root;
  root["version"] = 1;

  nlohmann::json remotes = nlohmann::json::array();
  for (const auto &r : remotes_)
  {
    remotes.push_back({{"name", r.name},
                       {"url", r.url},
                       {"username", r.username},
                       {"auth", authToString(r.auth)},
                       {"secret", util::obfuscate(r.secret)},
                       {"passphrase", util::obfuscate(r.passphrase)}});
  }
  root["remotes"] = remotes;

  nlohmann::json repos = nlohmann::json::array();
  for (const auto &r : repositories_)
    repos.push_back({{"name", r.name}, {"path", r.path}});
  root["repositories"] = repos;

  nlohmann::json hostKeys = nlohmann::json::object();
  for (const auto &item : hostKeys_) hostKeys[item.first] = item.second;
  root["host_keys"] = hostKeys;

  root["current_repository"] = currentRepository_;

  const std::string path = filePath();
  util::makeDirs(util::dirName(path));

  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << root.dump(2) << "\n";
  return out.good();
}

const std::string *Store::hostKey(const std::string &host) const
{
  const auto it = hostKeys_.find(host);
  return it == hostKeys_.end() ? nullptr : &it->second;
}

void Store::setHostKey(const std::string &host, const std::string &fingerprint)
{
  ensureLoaded();
  hostKeys_[host] = fingerprint;
}

const Repository *Store::findRepository(const std::string &path) const
{
  const std::string wanted = util::stripTrailingSep(util::normalizeSlashes(path));
  for (const auto &repo : repositories_)
  {
    if (util::stripTrailingSep(util::normalizeSlashes(repo.path)) == wanted) return &repo;
  }
  return nullptr;
}

const Remote *Store::findRemoteForUrl(const std::string &url) const
{
  const std::string lowered = util::toLower(url);
  for (const auto &remote : remotes_)
  {
    if (util::toLower(remote.url) == lowered) return &remote;
  }
  // Fall back to host-based matching: cloning a different path from a server
  // whose credentials we already know should reuse them.
  const std::string host = util::toLower(hostOf(url));
  if (host.empty()) return nullptr;
  for (const auto &remote : remotes_)
  {
    if (util::toLower(hostOf(remote.url)) == host) return &remote;
  }
  return nullptr;
}

Remote *Store::findRemote(const std::string &name)
{
  for (auto &remote : remotes_)
  {
    if (remote.name == name) return &remote;
  }
  return nullptr;
}

bool Store::removeRemote(const std::string &name)
{
  for (auto it = remotes_.begin(); it != remotes_.end(); ++it)
  {
    if (it->name == name)
    {
      remotes_.erase(it);
      return true;
    }
  }
  return false;
}

bool Store::removeRepository(const std::string &path)
{
  const std::string wanted = util::stripTrailingSep(util::normalizeSlashes(path));
  for (auto it = repositories_.begin(); it != repositories_.end(); ++it)
  {
    if (util::stripTrailingSep(util::normalizeSlashes(it->path)) == wanted)
    {
      if (currentRepository_ == it->path) currentRepository_.clear();
      repositories_.erase(it);
      return true;
    }
  }
  return false;
}

} // namespace settings
