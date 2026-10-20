// The File > Git > Panel window and the actions reachable from the menu.
#pragma once

#include <string>

namespace panel
{

// Creates the panel if needed and brings it to front.
void open();

bool isOpen();
void close();

// Re-reads remotes/repositories from the settings and the repository state.
void refresh();

// Cheap poll (called from a REAPER timer): refreshes the panel when the open
// project changed.
void pollProjectChange();

// Status line at the bottom of the panel.
void setStatus(const std::string &message);

} // namespace panel
