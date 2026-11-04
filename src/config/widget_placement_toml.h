#pragma once

#include "config/config_types.h"
#include "core/toml.h"

#include <string_view>
#include <vector>

// The [desktop_widgets]/[lockscreen_widgets] widget table format, shared with the
// wallpaper-bound widget sets stored under [desktop_widget_sets].
namespace noctalia::config {

  [[nodiscard]] toml::table desktopWidgetStateTable(const DesktopWidgetState& widget);

  // Writes `widget` + `widget_order` into `sectionTbl` (no grid: a set captures placement only).
  void writeDesktopWidgetPlacement(toml::table& sectionTbl, const std::vector<DesktopWidgetState>& widgets);

  // Reads that same shape. A missing grid table leaves `grid` untouched.
  bool readDesktopWidgetPlacement(
      const toml::table& sectionTbl, DesktopWidgetsGridState& grid, std::vector<DesktopWidgetState>& widgets,
      std::string_view colorSection
  );

} // namespace noctalia::config
