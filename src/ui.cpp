#include "ui.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#ifdef _WIN32
// WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in the common dialogs.
#  include <commdlg.h>
#  include <shlobj.h>
#endif

namespace ui
{

namespace
{

#ifdef _WIN32
std::wstring widen(const std::string &s)
{
  if (s.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
  return out;
}

std::string narrow(const std::wstring &s)
{
  if (s.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                                         0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed, nullptr,
                      nullptr);
  return out;
}

// Template with no items; the dialog proc builds the controls. `cx`/`cy` are
// in dialog units, the pixel size is applied in WM_INITDIALOG.
#pragma pack(push, 1)
struct BlankDialogTemplate
{
  DLGTEMPLATE dlg;
  WORD menu;
  WORD windowClass;
  WCHAR title;
};
#pragma pack(pop)

BlankDialogTemplate makeTemplate()
{
  BlankDialogTemplate tmpl{};
  // WS_CLIPCHILDREN stops the background from painting over the controls.
  tmpl.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME;
  tmpl.dlg.cdit = 0;
  tmpl.dlg.cx = 200;
  tmpl.dlg.cy = 120;
  return tmpl;
}
#endif

// The builder of the dialog currently being created. Dialogs are never nested,
// so one slot is enough.
BuildProc g_builder = nullptr;

#ifndef _WIN32
// SWELL asks the dialog resource to create the controls. It runs before
// WM_INITDIALOG, with the window already at its final size.
void swellBuildThunk(HWND hwnd, int /*flags*/)
{
  SWELL_MakeSetCurParms(1, 1, 0, 0, hwnd, false, false);
  if (g_builder) g_builder(hwnd);
}

SWELL_DialogResourceIndex makeResource(const char *title, int width, int height,
                                       SWELL_DialogResourceIndex *storage)
{
  storage->resid = reinterpret_cast<const char *>(storage);
  storage->title = title;
  storage->windowTypeFlags = 0; // top-level, fixed size
  storage->createFunc = swellBuildThunk;
  storage->width = width;
  storage->height = height;
  storage->_next = nullptr;
  return *storage;
}
#endif

} // namespace

INT_PTR showModal(HWND parent, const char *title, int width, int height, BuildProc build,
                  DLGPROC proc, LPARAM param)
{
  g_builder = build;
#ifdef _WIN32
  BlankDialogTemplate tmpl = makeTemplate();
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dlg, parent, proc, param);
#else
  SWELL_DialogResourceIndex storage{};
  makeResource(title, width, height, &storage);
  return SWELL_DialogBox(&storage, storage.resid, parent, proc, param);
#endif
}

HWND createWindow(HWND parent, const char *title, int width, int height, BuildProc build,
                  DLGPROC proc, LPARAM param)
{
  g_builder = build;
#ifdef _WIN32
  BlankDialogTemplate tmpl = makeTemplate();
  // The procedure creates the controls and sizes the window in WM_INITDIALOG.
  HWND hwnd = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dlg, parent, proc, param);
  if (!hwnd) return nullptr;
  ShowWindow(hwnd, SW_SHOW);
  return hwnd;
#else
  SWELL_DialogResourceIndex storage{};
  makeResource(title, width, height, &storage);
  HWND hwnd = SWELL_CreateDialog(&storage, storage.resid, parent, proc, param);
  if (hwnd) ShowWindow(hwnd, SW_SHOW);
  return hwnd;
#endif
}

void runBuilder(HWND hwnd)
{
#ifdef _WIN32
  if (g_builder) g_builder(hwnd);
#else
  (void)hwnd; // SWELL ran the builder through createFunc already
#endif
}

bool isInitMessage(UINT msg)
{
#ifdef _WIN32
  // A Win32 dialog procedure receives WM_CREATE and then WM_INITDIALOG. Treating
  // both as "initialise" built every control twice, leaving an untracked second
  // set that never moved when the panel rearranged itself.
  return msg == WM_INITDIALOG;
#else
  // SWELL announces its dialogs with WM_CREATE (and WM_INITDIALOG for some).
  return msg == WM_INITDIALOG || msg == WM_CREATE;
#endif
}

void refreshWindow(HWND hwnd)
{
  if (!hwnd) return;
#ifdef _WIN32
  // Windows invalidates the given window only, not its children: without
  // RDW_ALLCHILDREN controls keep their old pixels until the mouse hovers them.
  // No forced repaint either, otherwise a layout pass flickers.
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
#else
  // SWELL repaints from its own invalidation bookkeeping, children included.
  InvalidateRect(hwnd, nullptr, TRUE);
#endif
}

void applyInitialSize(HWND hwnd, const std::string &title, int width, int height)
{
  if (!hwnd) return;
#ifdef _WIN32
  SetWindowTextW(hwnd, widen(title).c_str());
  RECT rect{0, 0, width, height};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  AdjustWindowRectEx(&rect, style, FALSE, exStyle);
  SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
               SWP_NOMOVE | SWP_NOZORDER);
#else
  SetWindowText(hwnd, title.c_str());
  // SWELL already created the window with the resource's width/height; only
  // re-apply it if it differs (resizing resets the paint state).
  RECT rect{};
  GetClientRect(hwnd, &rect);
  if (rect.right - rect.left != width || rect.bottom - rect.top != height)
    SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
#endif
}

HWND addControl(HWND parent, const char *className, const std::string &text, int id, DWORD style,
                int x, int y, int w, int h)
{
#ifdef _WIN32
  const std::wstring klass = widen(className);
  const std::wstring caption = widen(text);
  return CreateWindowExW(0, klass.c_str(), caption.c_str(), style | WS_CHILD | WS_VISIBLE, x, y, w,
                         h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
#else
  SWELL_MakeSetCurParms(1, 1, 0, 0, parent, false, false);
  return SWELL_MakeControl(text.c_str(), id, className, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                           0);
#endif
}

// Dialog buttons are owner-drawn with the panel palette (see themeMessage), so
// dialogs opened from the panel look like the panel.
HWND addButton(HWND parent, const std::string &text, int id, int x, int y, int w, int h)
{
  return addControl(parent, "BUTTON", text, id, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h);
}

void moveControl(HWND control, int x, int y, int w, int h)
{
  if (!control) return;
  // A layout pass moves dozens of controls and ends with one refreshWindow call.
  SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void setText(HWND control, const std::string &text)
{
  if (!control) return;
#ifdef _WIN32
  SetWindowTextW(control, widen(text).c_str());
#else
  SetWindowText(control, text.c_str());
#endif
}

HFONT createFont(int height, bool bold)
{
  return CreateFont(-height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                    DEFAULT_PITCH, "sans-serif");
}

void setFont(HWND control, HFONT font)
{
  if (control && font) SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HBRUSH brush(COLORREF color)
{
  static std::map<COLORREF, HBRUSH> cache;
  auto it = cache.find(color);
  if (it != cache.end()) return it->second;

  HBRUSH created = CreateSolidBrush(color);
  cache[color] = created;
  return created;
}

void fillRect(HDC dc, const RECT &rect, HBRUSH brushHandle)
{
  if (!dc || !brushHandle) return;
  RECT copy = rect;
  ::FillRect(dc, &copy, brushHandle);
}

void drawCenteredText(HDC dc, const RECT &rect, const std::string &value)
{
  if (!dc) return;
  RECT copy = rect;
  SetBkMode(dc, TRANSPARENT);
#ifdef _WIN32
  DrawTextW(dc, widen(value).c_str(), -1, &copy, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
#else
  DrawText(dc, value.c_str(), -1, &copy, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
#endif
}

// Dialog chrome: same dark palette as the panel, so dialogs opened from the
// panel do not flash a light window.
namespace
{
const COLORREF kDialogBackground = RGB(38, 38, 38);
const COLORREF kDialogText = RGB(200, 200, 200);
const COLORREF kDialogButtonFace = RGB(200, 200, 200);
const COLORREF kDialogButtonFaceOff = RGB(80, 80, 80);
const COLORREF kDialogButtonText = RGB(0, 0, 0);
const COLORREF kDialogButtonTextOff = RGB(140, 140, 140);
// Warning rows (for example the irreversible "delete the folder" toggle) are
// drawn as light yellow text on the dialog background.
const COLORREF kDialogWarningText = RGB(255, 232, 120);

// Controls whose text is painted as a warning (light yellow).
std::set<HWND> &warningControls()
{
  static std::set<HWND> controls;
  return controls;
}
} // namespace

void setWarningText(HWND control, bool warning)
{
  if (!control) return;
  if (warning)
    warningControls().insert(control);
  else
    warningControls().erase(control);
}

bool themeMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
    {
      // wParam is the device context and the window is the one that received
      // the message; reading the window from lParam painted nothing.
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT client{};
      GetClientRect(hwnd, &client);
      fillRect(dc, client, brush(kDialogBackground));
      if (result) *result = 1;
      return true;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    {
      HDC dc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);
      const bool warning = control && warningControls().count(control) > 0;
      if (dc)
      {
        SetBkColor(dc, kDialogBackground);
        SetTextColor(dc, warning ? kDialogWarningText : kDialogText);
      }
      if (result) *result = reinterpret_cast<LRESULT>(brush(kDialogBackground));
      return true;
    }
    case WM_DRAWITEM:
    {
      // Owner-drawn dialog buttons: same face/text colours as the panel.
      // Buttons registered as warnings are drawn as yellow text on the
      // dialog background instead, so the whole row reads as a warning.
      auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
      if (!item || item->CtlType != ODT_BUTTON) return false;

      const bool enabled = IsWindowEnabled(item->hwndItem) != 0;
      fillRect(item->hDC, item->rcItem,
               brush(enabled ? kDialogButtonFace : kDialogButtonFaceOff));
      SetTextColor(item->hDC, enabled ? kDialogButtonText : kDialogButtonTextOff);
      drawCenteredText(item->hDC, item->rcItem, text(item->hwndItem));
      if (result) *result = 1;
      return true;
    }
    default:
      return false;
  }
}

std::string text(HWND control)
{
  if (!control) return {};
#ifdef _WIN32
  const int len = GetWindowTextLengthW(control);
  if (len <= 0) return {};
  std::wstring buffer(static_cast<std::size_t>(len) + 1, L'\0');
  GetWindowTextW(control, &buffer[0], len + 1);
  buffer.resize(static_cast<std::size_t>(len));
  return narrow(buffer);
#else
  char buffer[4096] = {0};
  GetWindowText(control, buffer, static_cast<int>(sizeof(buffer)));
  return buffer;
#endif
}

void setEnabled(HWND control, bool enabled)
{
  if (control) EnableWindow(control, enabled ? 1 : 0);
}

void setVisible(HWND control, bool visible)
{
  if (!control) return;
  // The layout pass repaints afterwards, covering the area a hidden one leaves.
  ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

int addComboItem(HWND combo, const std::string &text)
{
  if (!combo) return -1;
#ifdef _WIN32
  return static_cast<int>(
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(text).c_str())));
#else
  return static_cast<int>(
      SendMessage(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
#endif
}

int comboSelection(HWND combo)
{
  if (!combo) return -1;
  return static_cast<int>(SendMessage(combo, CB_GETCURSEL, 0, 0));
}

void comboSelect(HWND combo, int index)
{
  if (combo) SendMessage(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void comboClear(HWND combo)
{
  if (combo) SendMessage(combo, CB_RESETCONTENT, 0, 0);
}

int comboHeight()
{
#ifdef _WIN32
  return 220; // includes the dropped-down list
#else
  return 24; // SWELL opens its own drop-down list, the control stays one row high
#endif
}

void listAddColumn(HWND list, int index, const std::string &title, int width)
{
  if (!list) return;
#ifdef _WIN32
  const std::wstring titleW = widen(title);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.cx = width;
  column.iSubItem = index;
  column.pszText = const_cast<wchar_t *>(titleW.c_str());
  SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
               reinterpret_cast<LPARAM>(&column));
#else
  LVCOLUMN column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH;
  column.cx = width;
  column.iSubItem = index;
  column.pszText = const_cast<char *>(title.c_str());
  ListView_InsertColumn(list, index, &column);
#endif
}

int listAddRow(HWND list, const std::vector<std::string> &cells)
{
  if (!list) return -1;
  int row = -1;
#ifdef _WIN32
  std::wstring first = cells.empty() ? std::wstring() : widen(cells[0]);
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = 0x7FFFFFFF;
  item.pszText = const_cast<wchar_t *>(first.c_str());
  row = static_cast<int>(SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
  for (std::size_t i = 1; i < cells.size(); ++i)
  {
    std::wstring cell = widen(cells[i]);
    LVITEMW sub{};
    sub.iSubItem = static_cast<int>(i);
    sub.pszText = const_cast<wchar_t *>(cell.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&sub));
  }
#else
  LVITEM item{};
  item.mask = LVIF_TEXT;
  item.iItem = 0x7FFFFFFF;
  item.pszText = const_cast<char *>(cells.empty() ? "" : cells[0].c_str());
  row = ListView_InsertItem(list, &item);
  for (std::size_t i = 1; i < cells.size(); ++i)
    ListView_SetItemText(list, row, static_cast<int>(i), const_cast<char *>(cells[i].c_str()));
#endif
  InvalidateRect(list, nullptr, TRUE);
  return row;
}

void listClear(HWND list)
{
  if (!list) return;
#ifdef _WIN32
  SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
#else
  ListView_DeleteAllItems(list);
#endif
  InvalidateRect(list, nullptr, TRUE);
}

int listSelection(HWND list)
{
  if (!list) return -1;
  return static_cast<int>(ListView_GetNextItem(list, -1, LVNI_SELECTED));
}

void listSelect(HWND list, int row)
{
  if (!list) return;
#ifdef _WIN32
  LVITEMW item{};
  item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
  item.state = LVIS_SELECTED | LVIS_FOCUSED;
  SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
#else
  ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
#endif
}

// ---------------------------------------------------------------------------
// Generic dialogs
// ---------------------------------------------------------------------------

void alert(HWND parent, const std::string &title, const std::string &message)
{
  MessageBox(parent, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

bool confirm(HWND parent, const std::string &title, const std::string &message)
{
  return MessageBox(parent, message.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
}

namespace
{

struct PromptState
{
  std::string title;
  std::string label;
  std::string value;
  bool password = false;
  bool accepted = false;
  HWND edit = nullptr;
};

PromptState *g_promptState = nullptr;

void buildPromptDialog(HWND hwnd)
{
  PromptState *state = g_promptState;
  if (!state) return;
  addControl(hwnd, "STATIC", state->label, 0, SS_LEFT, 14, 14, 400, 18);
  state->edit = addControl(hwnd, "EDIT", state->value, 100,
                           WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL |
                               (state->password ? ES_PASSWORD : 0),
                           14, 40, 400, 24);
  addButton(hwnd, "OK", IDOK, 256, 88, 76, 26);
  addButton(hwnd, "Cancel", IDCANCEL, 338, 88, 76, 26);
}

UI_DLGRET promptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (isInitMessage(msg))
  {
    (void)lParam;
    PromptState *state = g_promptState;
    if (!state) return 0;
    runBuilder(hwnd);
    applyInitialSize(hwnd, state->title, 430, 140);
    if (state->edit) SetFocus(state->edit);
    return 0;
  }

  PromptState *state = g_promptState;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == IDOK)
    {
      state->value = text(state->edit);
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

struct CheckboxState
{
  std::string title;
  std::string message;
  std::string checkLabel;
  bool checked = false;
  bool accepted = false;
  HWND check = nullptr;
  // The checkbox stays a system control (SWELL paints it with its own colours),
  // so its label lives in a separate static, which can be coloured.
  HWND checkText = nullptr;
};

CheckboxState *g_checkboxState = nullptr;

void buildCheckboxDialog(HWND hwnd)
{
  CheckboxState *state = g_checkboxState;
  if (!state) return;
  addControl(hwnd, "STATIC", state->message, 0, SS_LEFT, 14, 14, 545, 60);
  // System checkbox (box only) plus a static label: SWELL paints a checkbox's
  // caption with its own theme colours, so the yellow warning text has to live in
  // a control whose colour the dialog controls.
  state->check =
      addControl(hwnd, "BUTTON", "", 101, BS_AUTOCHECKBOX | WS_TABSTOP, 14, 84, 22, 22);
  if (state->checked) SendMessage(state->check, BM_SETCHECK, BST_CHECKED, 0);
  state->checkText = addControl(hwnd, "STATIC", state->checkLabel, 0, SS_LEFT, 40, 84, 520, 44);
  setWarningText(state->checkText, true);
  addButton(hwnd, "Remove", IDOK, 404, 150, 76, 26);
  addButton(hwnd, "Cancel", IDCANCEL, 486, 150, 76, 26);
}

UI_DLGRET checkboxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (isInitMessage(msg))
  {
    (void)lParam;
    CheckboxState *state = g_checkboxState;
    if (!state) return 0;
    runBuilder(hwnd);
    applyInitialSize(hwnd, state->title, 580, 200);
    return 0;
  }

  CheckboxState *state = g_checkboxState;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == IDOK)
    {
      state->accepted = true;
      state->checked = SendMessage(state->check, BM_GETCHECK, 0, 0) == BST_CHECKED;
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

struct CommitOrStashState
{
  std::string title;
  std::string question;
  std::string message;
  bool stash = false;
  bool accepted = false;
  HWND edit = nullptr;
};

CommitOrStashState *g_commitOrStashState = nullptr;

void buildCommitOrStashDialog(HWND hwnd)
{
  CommitOrStashState *state = g_commitOrStashState;
  if (!state) return;
  addControl(hwnd, "STATIC", state->question, 0, SS_LEFT, 14, 14, 470, 40);
  addControl(hwnd, "STATIC", "Commit message:", 0, SS_LEFT, 14, 62, 220, 18);
  state->edit = addControl(hwnd, "EDIT", state->message, 100,
                           WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 82, 470, 24);
  addButton(hwnd, "Commit", IDOK, 246, 156, 76, 26);
  addButton(hwnd, "Stash", 102, 330, 156, 76, 26);
  addButton(hwnd, "Cancel", IDCANCEL, 414, 156, 76, 26);
}

UI_DLGRET commitOrStashProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT themed = 0;
  if (themeMessage(hwnd, msg, wParam, lParam, &themed)) return themed;

  if (isInitMessage(msg))
  {
    (void)lParam;
    CommitOrStashState *state = g_commitOrStashState;
    if (!state) return 0;
    runBuilder(hwnd);
    applyInitialSize(hwnd, state->title, 500, 220);
    if (state->edit) SetFocus(state->edit);
    return 0;
  }

  CommitOrStashState *state = g_commitOrStashState;
  if (!state) return 0;

  if (msg == WM_COMMAND)
  {
    const int id = LOWORD(wParam);
    if (id == IDOK || id == 102)
    {
      state->message = text(state->edit);
      state->stash = (id == 102);
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

} // namespace

bool prompt(HWND parent, const std::string &title, const std::string &label, std::string &value,
            bool password)
{
  PromptState state;
  state.title = title;
  state.label = label;
  state.value = value;
  state.password = password;
  g_promptState = &state;
  showModal(parent, title.c_str(), 430, 140, buildPromptDialog, promptProc,
            reinterpret_cast<LPARAM>(&state));
  g_promptState = nullptr;
  if (!state.accepted) return false;
  value = state.value;
  return true;
}

bool confirmWithCheckbox(HWND parent, const std::string &title, const std::string &message,
                         const std::string &checkLabel, bool *checked)
{
  CheckboxState state;
  state.title = title;
  state.message = message;
  state.checkLabel = checkLabel;
  state.checked = checked && *checked;
  g_checkboxState = &state;
  showModal(parent, title.c_str(), 580, 200, buildCheckboxDialog, checkboxProc,
            reinterpret_cast<LPARAM>(&state));
  g_checkboxState = nullptr;
  // The window is gone: drop the warning registration so its handle cannot be
  // matched by a later dialog.
  setWarningText(state.checkText, false);
  state.check = nullptr;
  state.checkText = nullptr;
  if (!state.accepted) return false;
  if (checked) *checked = state.checked;
  return true;
}

bool pickDirectory(HWND parent, const std::string &title, const std::string &initial,
                   std::string &path)
{
#ifdef _WIN32
  const std::wstring titleW = widen(title);
  wchar_t buffer[MAX_PATH] = {0};
  BROWSEINFOW info{};
  info.hwndOwner = parent;
  info.lpszTitle = titleW.c_str();
  info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE idList = SHBrowseForFolderW(&info);
  if (!idList) return false;
  const bool ok = SHGetPathFromIDListW(idList, buffer) != FALSE;
  CoTaskMemFree(idList);
  if (!ok) return false;
  path = narrow(buffer);
  return true;
#else
  char buffer[4096] = {0};
  const char *start = initial.empty() ? nullptr : initial.c_str();
  if (!BrowseForDirectory(title.c_str(), start, buffer, static_cast<int>(sizeof(buffer))))
    return false;
  if (!buffer[0]) return false;
  path = buffer;
  return true;
#endif
}

bool pickFile(HWND parent, const std::string &title, const std::string &initial,
              std::string &path)
{
#ifdef _WIN32
  const std::wstring titleW = widen(title);
  const std::wstring initialW = widen(initial);
  wchar_t buffer[MAX_PATH] = {0};
  if (!initialW.empty())
  {
    const std::size_t count =
        initialW.size() < MAX_PATH ? initialW.size() : static_cast<std::size_t>(MAX_PATH - 1);
    std::copy_n(initialW.c_str(), count, buffer);
  }

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = parent;
  dialog.lpstrTitle = titleW.c_str();
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

  if (!GetOpenFileNameW(&dialog)) return false;
  path = narrow(buffer);
  return !path.empty();
#else
  char *chosen = BrowseForFiles(title.c_str(), initial.empty() ? nullptr : initial.c_str(),
                                nullptr, false, nullptr);
  if (!chosen) return false;
  path = chosen;
  std::free(chosen); // SWELL allocates the result
  return !path.empty();
#endif
}

bool commitOrStash(HWND parent, const std::string &title, const std::string &question,
                   bool *stashOut, std::string *commitMessage)
{
  CommitOrStashState state;
  state.title = title;
  state.question = question;
  state.message = commitMessage ? *commitMessage : std::string();
  g_commitOrStashState = &state;
  showModal(parent, title.c_str(), 500, 220, buildCommitOrStashDialog, commitOrStashProc,
            reinterpret_cast<LPARAM>(&state));
  g_commitOrStashState = nullptr;
  if (!state.accepted) return false;
  if (stashOut) *stashOut = state.stash;
  if (commitMessage) *commitMessage = state.message;
  return true;
}

} // namespace ui
