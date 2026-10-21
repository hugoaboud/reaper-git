// Cross-platform dialog layer.
//
// Windows: dialogs from an in-memory template, controls created with
// CreateWindowExW.
//
// Linux: REAPER's SWELL. A dialog is described by a SWELL_DialogResourceIndex
// (the same structure swell_resgen generates from an .rc file) whose
// createFunc builds the controls. That is SWELL's supported path: it owns the
// window procedure, paints the dialog background, handles Enter/Esc/Tab and
// sizes the window, so the extension never has to repaint anything by hand.
#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <commctrl.h>
#else
#  include "swell/swell.h"
#  include "swell/swell-dlggen.h"
#endif

#ifdef _WIN32
#  define UI_DLGRET INT_PTR CALLBACK
#else
#  define UI_DLGRET INT_PTR
#endif

namespace ui
{

// Creates the dialog's child controls. Runs once, when the window exists.
using BuildProc = void (*)(HWND);

// Small helper fonts for the action hints (bold for the button titles).
HFONT createFont(int height, bool bold);
void setFont(HWND control, HFONT font);
// Font every control is created with. On Windows it is the system dialog font,
// so text has the same size as in any other Windows dialog; on Linux SWELL picks
// its own and this returns nullptr.
HFONT dialogFont();
// Height of one line of `font` in pixels, and the height `text` needs when it is
// wrapped at `width`. Both return 0 when the metric cannot be read.
int lineHeight(HFONT font);
int textHeight(const std::string &text, int width, HFONT font);
// Resizes a dialog or window so that the content fits.
void resizeWindow(HWND hwnd, int width, int height);
// Makes a list view select a whole row instead of a single cell.
void listSelectFullRows(HWND list);
// Invalidates the window *and every control in it*. This repaints everything, so
// it is for the end of a batch (a project load, for example), never per frame.
void refreshWindowDeep(HWND window);


// Solid brushes for the custom-drawn panel chrome (cached per colour).
HBRUSH brush(COLORREF color);
// Fills a rectangle with a brush.
void fillRect(HDC dc, const RECT &rect, HBRUSH brush);
// Draws the caption of `control` centred in the given rectangle, using the
// control's own font.
void drawCenteredText(HDC dc, const RECT &rect, HWND control);

// Dark dialog styling shared by every dialog: returns true when the message was
// a colour request (the caller must then return `result`).
bool themeMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result);

// Marks a static control to be painted as light yellow warning text on the
// dialog background (used for the label next to an irreversible toggle).
void setWarningText(HWND control, bool warning);

// Modal dialog; returns the value passed to EndDialog().
INT_PTR showModal(HWND parent, const char *title, int width, int height, BuildProc build,
                  DLGPROC proc, LPARAM param);

// Modeless top-level window; returns its HWND (or nullptr).
HWND createWindow(HWND parent, const char *title, int width, int height, BuildProc build,
                  DLGPROC proc, LPARAM param);

// Call from a dialog proc's initialization branch. On Windows the controls are
// built here; on Linux SWELL already built them while creating the window.
void runBuilder(HWND hwnd);

// True for the message that initializes a dialog (WM_INITDIALOG everywhere;
// WM_CREATE is accepted for robustness).
bool isInitMessage(UINT msg);

// Marks the whole window as needing a repaint (SWELL repaints children too).
void refreshWindow(HWND hwnd);

// Applies the title/size (pixels). Needed on Windows, harmless on Linux.
void applyInitialSize(HWND hwnd, const std::string &title, int width, int height);

// Control helpers. `className` is a Win32 class name: "BUTTON", "EDIT",
// "STATIC", "COMBOBOX", "SysListView32".
HWND addControl(HWND parent, const char *className, const std::string &text, int id, DWORD style,
                int x, int y, int w, int h);
// Owner-drawn push button (same look as the panel's action buttons).
HWND addButton(HWND parent, const std::string &text, int id, int x, int y, int w, int h);
void moveControl(HWND control, int x, int y, int w, int h);
void setText(HWND control, const std::string &text);
std::string text(HWND control);
void setEnabled(HWND control, bool enabled);
void setVisible(HWND control, bool visible);

int addComboItem(HWND combo, const std::string &text);
int comboSelection(HWND combo);
void comboSelect(HWND combo, int index);
void comboClear(HWND combo);

// Height to use when creating/moving a combo box. Win32 combo boxes need the
// height of the dropped-down list; SWELL's combo is just the closed box.
int comboHeight();

void listAddColumn(HWND list, int index, const std::string &title, int width);
int listAddRow(HWND list, const std::vector<std::string> &cells);
void listClear(HWND list);
int listSelection(HWND list);
void listSelect(HWND list, int row);

// Generic dialogs, reused everywhere instead of re-creating ad-hoc boxes.
void alert(HWND parent, const std::string &title, const std::string &message);
bool confirm(HWND parent, const std::string &title, const std::string &message);

// Single-line text prompt. `password` masks the input.
bool prompt(HWND parent, const std::string &title, const std::string &label, std::string &value,
            bool password = false);

// Native folder chooser (SWELL's BrowseForDirectory on Linux, SHBrowseForFolder
// on Windows). Returns false when the user cancels.
bool pickDirectory(HWND parent, const std::string &title, const std::string &initial,
                   std::string &path);

// Native file chooser (used for the SSH private key). Returns false when the
// user cancels.
bool pickFile(HWND parent, const std::string &title, const std::string &initial,
              std::string &path);

// Confirmation with an extra checkbox (used for the irreversible repo delete).
bool confirmWithCheckbox(HWND parent, const std::string &title, const std::string &message,
                         const std::string &checkLabel, bool *checked);

// "Commit or stash?" question with a commit message field.
bool commitOrStash(HWND parent, const std::string &title, const std::string &question,
                   bool *stashOut, std::string *commitMessage);

} // namespace ui
