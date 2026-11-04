#include "config/desktop_widget_set.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>

namespace desktop_widgets {

  std::string normalizeWidgetSetWallpaperKey(std::string_view path) {
    if (path.empty()) {
      return {};
    }
    std::string normalized = std::filesystem::path(path).lexically_normal().string();
    while (normalized.size() > 1 && normalized.back() == '/') {
      normalized.pop_back();
    }
    return normalized;
  }

  std::string widgetSetScopeKey(std::string_view wallpaperKey, std::string_view outputKey) {
    return std::format("{}:{}", outputKey, wallpaperKey);
  }

  const DesktopWidgetSet* WidgetSetCatalog::find(std::string_view id) const {
    if (id.empty()) {
      return nullptr;
    }
    const auto it = std::ranges::find(sets, id, &DesktopWidgetSet::id);
    return it == sets.end() ? nullptr : &*it;
  }

  const DesktopWidgetSet*
  WidgetSetCatalog::findByName(std::string_view wallpaperKey, std::string_view outputKey, std::string_view name) const {
    if (name.empty()) {
      return nullptr;
    }
    const auto it = std::ranges::find_if(sets, [&](const DesktopWidgetSet& set) {
      return set.wallpaper == wallpaperKey && set.outputName == outputKey && set.name == name;
    });
    return it == sets.end() ? nullptr : &*it;
  }

  std::string WidgetSetCatalog::defaultIdFor(std::string_view wallpaperKey, std::string_view outputKey) const {
    const auto inScope = [&](const DesktopWidgetSet& set) {
      return set.wallpaper == wallpaperKey && set.outputName == outputKey;
    };
    if (const auto it = defaults.find(widgetSetScopeKey(wallpaperKey, outputKey)); it != defaults.end()) {
      if (const DesktopWidgetSet* set = find(it->second); set != nullptr && inScope(*set)) {
        return set->id;
      }
    }
    const auto first = std::ranges::find_if(sets, inScope);
    return first == sets.end() ? std::string() : first->id;
  }

  std::string WidgetSetCatalog::activeIdFor(std::string_view outputKey) const {
    const auto it = active.find(std::string(outputKey));
    return it == active.end() ? std::string() : it->second;
  }

  std::vector<WidgetSetView>
  WidgetSetCatalog::viewsFor(std::string_view wallpaperKey, std::string_view outputKey) const {
    const std::string defaultId = defaultIdFor(wallpaperKey, outputKey);
    const std::string activeId = activeIdFor(outputKey);
    std::vector<WidgetSetView> views;
    for (const auto& set : sets) {
      if (set.wallpaper != wallpaperKey || set.outputName != outputKey) {
        continue;
      }
      views.push_back(
          WidgetSetView{
              .id = set.id,
              .name = set.name,
              .isDefault = !defaultId.empty() && set.id == defaultId,
              .isActive = !activeId.empty() && set.id == activeId,
          }
      );
    }
    return views;
  }

  std::string WidgetSetCatalog::nextId() const {
    std::int32_t highest = 0;
    for (const auto& set : sets) {
      const auto dash = set.id.rfind('-');
      if (dash == std::string::npos || dash + 1 >= set.id.size()) {
        continue;
      }
      try {
        highest = std::max(highest, std::stoi(set.id.substr(dash + 1)));
      } catch (...) {
        continue;
      }
    }
    return std::format("widget-set-{:04d}", highest + 1);
  }

  const DesktopWidgetSet* selectSetForApply(
      const WidgetSetCatalog& catalog, std::string_view wallpaperKey, std::string_view outputKey,
      std::string_view requestedName
  ) {
    if (!requestedName.empty()) {
      return catalog.findByName(wallpaperKey, outputKey, requestedName);
    }
    return catalog.find(catalog.defaultIdFor(wallpaperKey, outputKey));
  }

  bool eraseWidgetSet(WidgetSetCatalog& catalog, std::string_view id) {
    const auto it = std::ranges::find(catalog.sets, id, &DesktopWidgetSet::id);
    if (it == catalog.sets.end()) {
      return false;
    }
    const std::string scope = widgetSetScopeKey(it->wallpaper, it->outputName);
    catalog.sets.erase(it);

    for (auto activeIt = catalog.active.begin(); activeIt != catalog.active.end();) {
      activeIt = activeIt->second == id ? catalog.active.erase(activeIt) : std::next(activeIt);
    }

    // defaultIdFor() falls back to the scope's first remaining set once the stale entry is gone.
    if (const auto defaultIt = catalog.defaults.find(scope);
        defaultIt != catalog.defaults.end() && defaultIt->second == id) {
      catalog.defaults.erase(defaultIt);
    }
    return true;
  }

  bool setDefaultWidgetSet(WidgetSetCatalog& catalog, std::string_view id) {
    const DesktopWidgetSet* set = catalog.find(id);
    if (set == nullptr) {
      return false;
    }
    catalog.defaults.insert_or_assign(widgetSetScopeKey(set->wallpaper, set->outputName), set->id);
    return true;
  }

  std::vector<DesktopWidgetState>
  widgetsForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey) {
    std::vector<DesktopWidgetState> result;
    for (const auto& widget : widgets) {
      if (widget.outputName == outputKey) {
        result.push_back(widget);
      }
    }
    return result;
  }

} // namespace desktop_widgets
