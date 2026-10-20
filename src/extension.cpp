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
#include <cstring>

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

// Called by REAPER while it initializes its default menus (flag == 0).
void addGitMenu(HMENU fileMenu)
{
  // A single File > Git entry that opens the panel; pull/push live there.
  // It goes first, as requested (the rest of the File menu follows it).
  addMenuItem(fileMenu, 0, g_panelCommand, "Git");
}

// REAPER calls this for every customizable menu, see "hookcustommenu" in
// reaper_plugin.h. Anything Reaper-owned may only be touched from here.
void menuHook(const char *menuId, void *menu, int flag)
{
  // flag 0: the default menu structure is being built. Add the submenu once,
  // it then becomes part of the user-customizable menu.
  if (flag != 0) return;
  if (!isMainFileMenu(menuId)) return;

  addGitMenu(static_cast<HMENU>(menu));
}

// Runs before every main-section action; return true when we handled it.
bool commandHook(int command, int flag)
{
  (void)flag;

  if (command == g_panelCommand)
  {
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
    unregisterAll();
    return 0;
  }

  g_rec = rec;

  if (REAPERAPI_LoadAPI(rec->GetFunc) != 0)
  {
    g_rec = nullptr;
    return 0; // API mismatch, let REAPER unload us.
  }

  git::init();

  g_panelCommand = plugin_register("command_id", const_cast<char *>(kPanelCommandId));
  if (!g_panelCommand) return 0;

  g_panelAccel.accel.cmd = static_cast<WORD>(g_panelCommand);

  if (!plugin_register("gaccel", &g_panelAccel)) return 0;

  if (!plugin_register("hookcommand", reinterpret_cast<void *>(commandHook))) return 0;
  if (!plugin_register("hookcustommenu", reinterpret_cast<void *>(menuHook))) return 0;

  // Notice project loads made through REAPER itself, so the Project section
  // always describes the open project.
  plugin_register("timer", reinterpret_cast<void *>(timerProc));

  plugin_register("ext_name", const_cast<char *>("reaper_git"));

  return 1;
}
