#pragma once

#include "config/config_types.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Wallpaper-bound desktop widget sets. The persistence layer lives in ConfigService
// (settings.toml, next to the widget placement it captures); everything here is pure so the
// selection/promotion rules can be tested without a shell.
namespace desktop_widgets {

  // Sets are keyed by wallpaper path. lexically_normal() folds `.` and `..` so the same file
  // referenced two ways lands in one bucket; it deliberately avoids canonical(), which would
  // touch the filesystem and fail for a wallpaper that moved away.
  [[nodiscard]] std::string normalizeWidgetSetWallpaperKey(std::string_view path);

  // Sets are scoped to one wallpaper on one output: the same picture on two monitors carries two
  // independent lists of sets, each with its own default. This joins both into the key the
  // defaults table is stored under.
  [[nodiscard]] std::string widgetSetScopeKey(std::string_view wallpaperKey, std::string_view outputKey);

  struct DesktopWidgetSet {
    std::string id;
    std::string name;
    std::string wallpaper;
    // Output the widgets were captured on; the set belongs to this output's wallpaper only.
    std::string outputName;
    std::vector<DesktopWidgetState> widgets;
    bool operator==(const DesktopWidgetSet&) const = default;
  };

  struct WidgetSetView {
    std::string id;
    std::string name;
    bool isDefault = false;
    bool isActive = false;
  };

  struct WidgetSetCatalog {
    std::vector<DesktopWidgetSet> sets;
    std::unordered_map<std::string, std::string> defaults; // widgetSetScopeKey -> set id
    std::unordered_map<std::string, std::string> active;   // output key -> set id

    [[nodiscard]] const DesktopWidgetSet* find(std::string_view id) const;
    [[nodiscard]] const DesktopWidgetSet*
    findByName(std::string_view wallpaperKey, std::string_view outputKey, std::string_view name) const;
    // The stored default of the scope, or its first set when none is stored (or the stored one is
    // gone), so a scope with sets always has a default. Empty only for a scope without sets.
    [[nodiscard]] std::string defaultIdFor(std::string_view wallpaperKey, std::string_view outputKey) const;
    [[nodiscard]] std::string activeIdFor(std::string_view outputKey) const;
    // Sets of one wallpaper on one output, in creation order, annotated with default/active.
    [[nodiscard]] std::vector<WidgetSetView> viewsFor(std::string_view wallpaperKey, std::string_view outputKey) const;
    [[nodiscard]] std::string nextId() const;
  };

  // The set a wallpaper change applies on one output: the requested one when named, else the
  // scope's default. nullptr means "no sets for this wallpaper on this output" (widgets stay as
  // they are) or that the requested name is unknown.
  [[nodiscard]] const DesktopWidgetSet* selectSetForApply(
      const WidgetSetCatalog& catalog, std::string_view wallpaperKey, std::string_view outputKey,
      std::string_view requestedName
  );

  // Removes a set; when it was its scope's default the stored default is dropped, so the first
  // remaining set of the scope takes over. Active markers pointing at it are dropped.
  bool eraseWidgetSet(WidgetSetCatalog& catalog, std::string_view id);

  // Points the default of the set's scope at the set. False for an unknown id.
  bool setDefaultWidgetSet(WidgetSetCatalog& catalog, std::string_view id);

  // Widgets belonging to one output, in snapshot order.
  [[nodiscard]] std::vector<DesktopWidgetState>
  widgetsForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey);

} // namespace desktop_widgets
