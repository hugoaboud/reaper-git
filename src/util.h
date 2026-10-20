// Small string/path/time helpers shared by the whole extension.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace util
{

inline std::string toLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline std::string trim(const std::string &s)
{
  const char *ws = " \t\r\n";
  const std::size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  const std::size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

inline bool startsWith(const std::string &s, const std::string &prefix)
{
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string &s, const std::string &suffix)
{
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string join(const std::vector<std::string> &parts, const char *sep)
{
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i)
  {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

inline std::vector<std::string> split(const std::string &s, char sep)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : s)
  {
    if (c == sep)
    {
      out.push_back(cur);
      cur.clear();
    }
    else
    {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

inline std::string replaceAll(std::string s, const std::string &from, const std::string &to)
{
  if (from.empty()) return s;
  std::string out;
  std::size_t pos = 0;
  while (true)
  {
    const std::size_t hit = s.find(from, pos);
    if (hit == std::string::npos)
    {
      out += s.substr(pos);
      break;
    }
    out += s.substr(pos, hit - pos);
    out += to;
    pos = hit + from.size();
  }
  return out;
}

#ifdef _WIN32
inline const char kPathSep = '\\';
inline const char kAltPathSep = '/';
#else
inline const char kPathSep = '/';
inline const char kAltPathSep = '/';
#endif

inline std::string normalizeSlashes(std::string path)
{
#ifdef _WIN32
  std::replace(path.begin(), path.end(), '/', '\\');
#endif
  return path;
}

inline bool isAbsolutePath(const std::string &path)
{
  if (path.empty()) return false;
#ifdef _WIN32
  return path.size() > 1 && path[1] == ':';
#else
  return path[0] == '/';
#endif
}

inline std::string pathJoin(const std::string &a, const std::string &b)
{
  if (a.empty()) return b;
  if (b.empty()) return a;
  std::string out = a;
  if (out.back() != '/' && out.back() != '\\') out.push_back(kPathSep);
  std::string rhs = b;
  while (!rhs.empty() && (rhs.front() == '/' || rhs.front() == '\\')) rhs.erase(rhs.begin());
  return normalizeSlashes(out + rhs);
}

inline std::string fileName(const std::string &path)
{
  const std::size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

inline std::string dirName(const std::string &path)
{
  const std::size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

inline std::string stripTrailingSep(std::string path)
{
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  return path;
}

inline std::string expandHome(const std::string &path)
{
  if (path.empty() || path[0] != '~') return path;
#ifdef _WIN32
  const char *home = std::getenv("USERPROFILE");
#else
  const char *home = std::getenv("HOME");
#endif
  if (!home) return path;
  return std::string(home) + path.substr(1);
}

inline bool fileExists(const std::string &path)
{
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

inline bool directoryExists(const std::string &path)
{
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  return (st.st_mode & S_IFDIR) != 0;
}

inline bool makeDirs(const std::string &path)
{
  if (path.empty() || directoryExists(path)) return true;
  const std::string parent = dirName(path);
  if (!parent.empty() && parent != path && !makeDirs(parent)) return false;
#ifdef _WIN32
  return ::_mkdir(path.c_str()) == 0;
#else
  return ::mkdir(path.c_str(), 0755) == 0;
#endif
}

inline std::string formatTime(std::time_t when)
{
  if (when <= 0) return "-";
  std::tm tmv {};
#ifdef _WIN32
  if (localtime_s(&tmv, &when) != 0) return "-";
#else
  if (!localtime_r(&when, &tmv)) return "-";
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
  return buf;
}

inline std::string truncate(const std::string &s, std::size_t maxLen)
{
  if (s.size() <= maxLen) return s;
  return s.substr(0, maxLen - 3) + "..."; // ASCII: SWELL's fonts may lack U+2026
}

inline std::string firstLine(const std::string &s)
{
  const std::size_t nl = s.find('\n');
  std::string out = nl == std::string::npos ? s : s.substr(0, nl);
  while (!out.empty() && (out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

// Reject characters that must not end up in the settings file / UI.
inline std::string sanitizeName(const std::string &s)
{
  std::string out;
  for (unsigned char c : s)
  {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@' || c == ' ')
      out.push_back(static_cast<char>(c));
    else
      out.push_back('_');
  }
  out = trim(out);
  return out.empty() ? std::string("remote") : out;
}

// ---------------------------------------------------------------------------
// Credential obfuscation.
//
// The spec asks for stored credentials that are not readable at a glance. This
// is a reversible XOR/hex scramble, *not* encryption: anyone with the file and
// this source can recover the secret.
// ---------------------------------------------------------------------------
inline std::string obfuscate(const std::string &plain)
{
  static const char kKey[] = "reaper-git/obfuscation-key/v1";
  static const unsigned char kHex[] = "0123456789abcdef";
  const std::size_t keyLen = sizeof(kKey) - 1;
  std::string out;
  out.reserve(plain.size() * 2);
  for (std::size_t i = 0; i < plain.size(); ++i)
  {
    const unsigned char v =
        static_cast<unsigned char>(plain[i]) ^ static_cast<unsigned char>(kKey[i % keyLen]) ^
        static_cast<unsigned char>((i * 31) & 0xFF);
    out.push_back(static_cast<char>(kHex[v >> 4]));
    out.push_back(static_cast<char>(kHex[v & 0xF]));
  }
  return out;
}

inline std::string deobfuscate(const std::string &encoded)
{
  static const char kKey[] = "reaper-git/obfuscation-key/v1";
  const std::size_t keyLen = sizeof(kKey) - 1;
  if (encoded.size() % 2 != 0) return {};
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(encoded.size() / 2);
  for (std::size_t i = 0; i < encoded.size(); i += 2)
  {
    const int hi = hexVal(encoded[i]);
    const int lo = hexVal(encoded[i + 1]);
    if (hi < 0 || lo < 0) return {};
    const unsigned char v = static_cast<unsigned char>((hi << 4) | lo);
    out.push_back(static_cast<char>(v ^ static_cast<unsigned char>(kKey[(i / 2) % keyLen]) ^
                                    static_cast<unsigned char>(((i / 2) * 31) & 0xFF)));
  }
  return out;
}

} // namespace util
