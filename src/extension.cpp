// reaper_git - REAPER extension embedding libgit2.
//
// Adds a single "Git" entry to REAPER's File menu which opens the git panel
// (fetch/pull/push live in the panel).
//
// Entry point is ReaperPluginEntry(), see third_party/reaper-sdk/reaper_plugin.h.

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include "swell/swell.h"
#endif

#define REAPERAPI_IMPLEMENT
#include "reaper_api.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "git_service.h"
#include "panel.h"

// SWELL (REAPER's Win32 emulation on Linux) only knows the older MIIM_TYPE
// spelling; real Win32 headers provide both.
#ifndef MIIM_STRING
#  define MIIM_STRING MIIM_TYPE
#endif

namespace
{

reaper_plugin_info_t *g_rec = nullptr;

int g_panelCommand = 0;

// Diagnostics. REAPER gives extensions no console, so the load sequence and the
// menu decisions are appended to one small file, truncated at every load. When
// the file is missing after starting REAPER, the module was never loaded (a
// missing runtime dependency, for example) - which is the first thing to know.
bool g_log_started = false;

std::string logFilePath()
{
  if (GetResourcePath)
  {
    const char *resource = GetResourcePath();
    if (resource && *resource) return std::string(resource) + "/reaper_git.log";
  }

  std::error_code error;
  const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (!error) return (temp / "reaper_git.log").string();
  return {};
}

void logLine(const std::string &text)
{
  const std::string path = logFilePath();
  if (path.empty()) return;

  const char *mode = g_log_started ? "a" : "w";
  g_log_started = true;
  if (FILE *file = std::fopen(path.c_str(), mode))
  {
    std::fprintf(file, "%s\n", text.c_str());
    std::fclose(file);
  }
}

const char *const kPanelCommandId = "REAPERGIT_PANEL";

// REAPER identifies the main File menu as "Main file". Compare loosely so the
// spelling/casing differences between REAPER versions do not matter.
bool isMainFileMenu(const char *menuId)
{
  if (!menuId) return false;

  char normalized[64];
  std::size_t n = 0;
  for (const char *p = menuId; *p && n + 1 < sizeof(normalized); ++p)
  {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == ' ' || c == '_' || c == '-') continue;
    normalized[n++] = static_cast<char>(std::tolower(c));
  }
  normalized[n] = '\0';

  return std::strcmp(normalized, "mainfile") == 0;
}

void addMenuItem(HMENU menu, int position, int command, const char *text)
{
  MENUITEMINFO item{};
  item.cbSize = sizeof(MENUITEMINFO);
  item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
  item.fType = MFT_STRING;
  item.fState = MFS_ENABLED;
  item.wID = static_cast<UINT>(command);
  item.dwTypeData = const_cast<char *>(text);

  InsertMenuItem(menu, position, TRUE, &item);
}

// True when the menu already contains our command. REAPER asks the hook before
// every display, and a stored menu customization never contains additions made
// to the default menu, so the entry has to be added again there; this keeps it
// from appearing twice when it is already present.
bool menuHasCommand(HMENU menu, int command)
{
  if (!menu) return false;

  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i)
  {
    if (static_cast<int>(GetMenuItemID(menu, i)) == command) return true;

    HMENU submenu = GetSubMenu(menu, i);
    if (submenu && menuHasCommand(submenu, command)) return true;
  }
  return false;
}

// A single File > Git entry that opens the panel; pull and push live there.
void addGitMenu(HMENU fileMenu)
{
  // It goes first, as requested (the rest of the File menu follows it).
  addMenuItem(fileMenu, 0, g_panelCommand, "Git");
}

// REAPER calls this for every customizable menu, see "hookcustommenu" in
// reaper_plugin.h. Anything REAPER-owned may only be touched from here.
//
// flag 0 means the default menu is being built, so whatever is added becomes
// part of the default structure the user can customize. flag 1 means the menu is
// about to be shown; a user who has customized their File menu gets only this
// call, and additions from flag 0 are not part of that stored structure. Both
// cases are therefore handled, adding the entry only when it is missing.
void menuHook(const char *menuId, void *menu, int flag)
{
  const std::string id = menuId ? menuId : "(null)";

  if (flag != 0 && flag != 1)
  {
    logLine("menu: id='" + id + "' flag=" + std::to_string(flag) + " ignored (flag)");
    return;
  }
  if (!isMainFileMenu(menuId))
  {
    logLine("menu: id='" + id + "' flag=" + std::to_string(flag) + " ignored (other menu)");
    return;
  }
  if (g_panelCommand <= 0)
  {
    logLine("menu: File menu seen before the command id was registered");
    return;
  }

  HMENU fileMenu = static_cast<HMENU>(menu);
  if (!fileMenu)
  {
    logLine("menu: File menu handle is null");
    return;
  }

  if (menuHasCommand(fileMenu, g_panelCommand))
  {
    logLine("menu: id='" + id + "' flag=" + std::to_string(flag) + " entry already present");
    return;
  }

  addGitMenu(fileMenu);
  logLine("menu: id='" + id + "' flag=" + std::to_string(flag) + " entry added");
}

// Runs before every main-section action; return true when we handled it.
bool commandHook(int command, int flag)
{
  (void)flag;

  if (command == g_panelCommand)
  {
    logLine("command: panel requested");
    panel::open();
    return true;
  }

  return false;
}

// No default keyboard shortcut; the accelerator exists so the action is listed
// (and bindable) in the Actions window.
gaccel_register_t g_panelAccel = {{0, 0, 0}, "Git: Panel"};

// REAPER calls this periodically on the main thread; the panel uses it to
// notice project loads done outside the extension.
void timerProc() { panel::pollProjectChange(); }

void unregisterAll()
{
  if (!plugin_register) return; // API never loaded, nothing to undo

  panel::close();

  plugin_register("-timer", reinterpret_cast<void *>(timerProc));
  plugin_register("-hookcustommenu", reinterpret_cast<void *>(menuHook));
  plugin_register("-hookcommand", reinterpret_cast<void *>(commandHook));
  plugin_register("-gaccel", &g_panelAccel);

  g_panelCommand = 0;
  g_rec = nullptr;

  git::shutdown();
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int
REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t *rec)
{
  (void)hInstance;

  if (!rec)
  {
    // REAPER is unloading the extension.
    logLine("entry: unload requested");
    unregisterAll();
    return 0;
  }

  g_rec = rec;

  logLine("entry: load, caller version " + std::to_string(rec->caller_version));

  const int apiResult = REAPERAPI_LoadAPI(rec->GetFunc);
  if (apiResult != 0)
  {
    logLine("entry: REAPERAPI_LoadAPI failed with " + std::to_string(apiResult) +
            " (missing function, module unloaded)");
    g_rec = nullptr;
    return 0; // API mismatch, let REAPER unload us.
  }
  logLine("entry: API loaded");

  git::init();

  g_panelCommand = plugin_register("command_id", const_cast<char *>(kPanelCommandId));
  if (!g_panelCommand)
  {
    logLine("entry: command_id registration failed");
    return 0;
  }
  logLine("entry: command id " + std::to_string(g_panelCommand));

  g_panelAccel.accel.cmd = static_cast<WORD>(g_panelCommand);

  if (!plugin_register("gaccel", &g_panelAccel))
  {
    logLine("entry: gaccel registration failed");
    return 0;
  }

  if (!plugin_register("hookcommand", reinterpret_cast<void *>(commandHook)))
  {
    logLine("entry: hookcommand registration failed");
    return 0;
  }
  if (!plugin_register("hookcustommenu", reinterpret_cast<void *>(menuHook)))
  {
    logLine("entry: hookcustommenu registration failed");
    return 0;
  }
  logLine("entry: hooks registered");

  // Notice project loads made through REAPER itself, so the Project section
  // always describes the open project.
  plugin_register("timer", reinterpret_cast<void *>(timerProc));

  plugin_register("ext_name", const_cast<char *>("reaper_git"));

  logLine("entry: ready");
  return 1;
}
