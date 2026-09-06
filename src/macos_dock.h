#pragma once

// The game runs as a bare executable, so its Dock tile has no icon unless one is set at runtime.
void macos_set_dock_icon(const char *iconPath);
