// Locks the wallpaper-bound widget set rules: how a wallpaper path is keyed, which set a
// wallpaper change picks, default promotion on delete, and that the catalog round-trips through
// settings.toml next to the widget placement it captures.

#include "config/config_service.h"
#include "config/config_types.h"
#include "config/desktop_widget_set.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  using desktop_widgets::DesktopWidgetSet;
  using desktop_widgets::WidgetSetCatalog;

  constexpr std::string_view kWallpaperA = "/wallpapers/mountain.png";
  constexpr std::string_view kWallpaperB = "/wallpapers/city.png";

  DesktopWidgetState makeWidget(std::string id, std::string output) {
    DesktopWidgetState widget;
    widget.id = std::move(id);
    widget.type = "clock";
    widget.outputName = std::move(output);
    return widget;
  }

  DesktopWidgetSet makeSet(std::string id, std::string name, std::string wallpaper, std::string output) {
    DesktopWidgetSet set;
    set.id = std::move(id);
    set.name = std::move(name);
    set.wallpaper = std::move(wallpaper);
    set.outputName = std::move(output);
    set.widgets.push_back(makeWidget("widget-1", set.outputName));
    return set;
  }

  void checkNormalize() {
    TEST_CHECK(desktop_widgets::normalizeWidgetSetWallpaperKey("/a/b/c.png") == "/a/b/c.png");
    TEST_CHECK(desktop_widgets::normalizeWidgetSetWallpaperKey("/a/./b/../b/c.png") == "/a/b/c.png");
    TEST_CHECK(desktop_widgets::normalizeWidgetSetWallpaperKey("/a/b/") == "/a/b");
    TEST_CHECK(desktop_widgets::normalizeWidgetSetWallpaperKey("") == "");
  }

  void checkCatalogRules() {
    WidgetSetCatalog catalog;
    catalog.sets.push_back(makeSet("widget-set-0001", "Calm", std::string(kWallpaperA), "DP-1"));
    catalog.sets.push_back(makeSet("widget-set-0002", "Busy", std::string(kWallpaperA), "DP-1"));
    catalog.sets.push_back(makeSet("widget-set-0003", "Night", std::string(kWallpaperB), "DP-2"));

    TEST_CHECK(catalog.nextId() == "widget-set-0004");
    TEST_CHECK(catalog.find("widget-set-0002") != nullptr);
    TEST_CHECK(catalog.findByName(kWallpaperA, "DP-1", "Busy") != nullptr);
    // Names are scoped to a wallpaper, so the same name may exist on another wallpaper.
    TEST_CHECK(catalog.findByName(kWallpaperB, "DP-1", "Busy") == nullptr);

    // No stored default: the scope's first set stands in, and a scope without sets yields nothing.
    TEST_CHECK(catalog.defaultIdFor(kWallpaperA, "DP-1") == "widget-set-0001");
    TEST_CHECK(desktop_widgets::selectSetForApply(catalog, kWallpaperA, "DP-2", "") == nullptr);
    // The same wallpaper on another output has its own sets: DP-1's are not applied there.
    TEST_CHECK(catalog.findByName(kWallpaperA, "DP-2", "Calm") == nullptr);
    TEST_CHECK(catalog.viewsFor(kWallpaperA, "DP-2").empty());

    TEST_CHECK(desktop_widgets::setDefaultWidgetSet(catalog, "widget-set-0002"));
    TEST_CHECK(catalog.defaultIdFor(kWallpaperA, "DP-1") == "widget-set-0002");
    const auto* applied = desktop_widgets::selectSetForApply(catalog, kWallpaperA, "DP-1", "");
    TEST_CHECK(applied != nullptr && applied->name == "Busy");
    // An explicit name wins over the default, and an unknown name resolves to nothing.
    applied = desktop_widgets::selectSetForApply(catalog, kWallpaperA, "DP-1", "Calm");
    TEST_CHECK(applied != nullptr && applied->id == "widget-set-0001");
    TEST_CHECK(desktop_widgets::selectSetForApply(catalog, kWallpaperA, "DP-1", "Missing") == nullptr);

    // Active marker is per output and shows up in the editor's view of one wallpaper.
    catalog.active.insert_or_assign("DP-1", "widget-set-0001");
    const auto views = catalog.viewsFor(kWallpaperA, "DP-1");
    TEST_CHECK(views.size() == 2);
    TEST_CHECK(views[0].name == "Calm" && views[0].isActive && !views[0].isDefault);
    TEST_CHECK(views[1].name == "Busy" && views[1].isDefault && !views[1].isActive);

    // Deleting the default promotes the first remaining set of that wallpaper and drops the marker.
    TEST_CHECK(desktop_widgets::eraseWidgetSet(catalog, "widget-set-0002"));
    TEST_CHECK(catalog.defaultIdFor(kWallpaperA, "DP-1") == "widget-set-0001");
    TEST_CHECK(catalog.activeIdFor("DP-1") == "widget-set-0001");
    // Deleting the last set of a wallpaper clears its default; deleting an unknown id is a no-op.
    TEST_CHECK(desktop_widgets::eraseWidgetSet(catalog, "widget-set-0003"));
    TEST_CHECK(catalog.defaultIdFor(kWallpaperB, "DP-2").empty());
    TEST_CHECK(!desktop_widgets::eraseWidgetSet(catalog, "widget-set-0003"));
    TEST_CHECK(!desktop_widgets::setDefaultWidgetSet(catalog, "widget-set-0003"));
  }

  void checkWidgetsForOutput() {
    const std::vector<DesktopWidgetState> widgets{
        makeWidget("a", "DP-1"),
        makeWidget("b", "DP-2"),
        makeWidget("c", "DP-1"),
    };
    const auto first = desktop_widgets::widgetsForOutput(widgets, "DP-1");
    TEST_CHECK(first.size() == 2);
    TEST_CHECK(first[0].id == "a" && first[1].id == "c");
    TEST_CHECK(desktop_widgets::widgetsForOutput(widgets, "DP-3").empty());
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  void checkPersistence() {
    const auto root = std::filesystem::temp_directory_path() / ("noctalia-widget-sets-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeFile(root / "config" / "noctalia.toml", "");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

    {
      ConfigService config;
      TEST_CHECK(config.desktopWidgetSets().sets.empty());

      // First save for a wallpaper becomes the default, and the id is assigned here.
      TEST_CHECK(config.saveDesktopWidgetSet(makeSet("", "Calm", std::string(kWallpaperA), "DP-1")));
      auto catalog = config.desktopWidgetSets();
      TEST_CHECK(catalog.sets.size() == 1);
      const std::string calmId = catalog.sets.front().id;
      TEST_CHECK(catalog.sets.front().widgets.size() == 1);
      TEST_CHECK(catalog.defaultIdFor(kWallpaperA, "DP-1") == calmId);

      // A second set for the same wallpaper does not steal the default.
      TEST_CHECK(config.saveDesktopWidgetSet(makeSet("", "Busy", std::string(kWallpaperA), "DP-1")));
      catalog = config.desktopWidgetSets();
      TEST_CHECK(catalog.sets.size() == 2);
      TEST_CHECK(catalog.defaultIdFor(kWallpaperA, "DP-1") == calmId);
      TEST_CHECK(catalog.sets[1].id != calmId);

      // Names are unique per wallpaper: a duplicate is refused instead of silently overwriting.
      TEST_CHECK(!config.saveDesktopWidgetSet(makeSet("", "Calm", std::string(kWallpaperA), "DP-1")));

      // Another wallpaper starts its own default.
      TEST_CHECK(config.saveDesktopWidgetSet(makeSet("", "Night", std::string(kWallpaperB), "DP-2")));

      // Active set per output survives a reload, and widgets are captured as written.
      TEST_CHECK(config.setActiveDesktopWidgetSet("DP-1", catalog.sets[1].id));
      TEST_CHECK(!config.setActiveDesktopWidgetSet("DP-1", "widget-set-9999"));

      // Re-saving an existing id replaces its widgets (the editor write-back path).
      DesktopWidgetSet edited = catalog.sets[0];
      edited.widgets.push_back(makeWidget("widget-2", "DP-1"));
      TEST_CHECK(config.saveDesktopWidgetSet(edited));
      TEST_CHECK(config.desktopWidgetSets().sets[0].widgets.size() == 2);
      TEST_CHECK(config.desktopWidgetSets().defaultIdFor(kWallpaperA, "DP-1") == calmId);
    }

    const std::string settings = [&] {
      std::ifstream in(root / "state" / "noctalia" / "settings.toml");
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    TEST_CHECK(settings.find("desktop_widget_sets") != std::string::npos);
    TEST_CHECK(settings.find(std::string(kWallpaperA)) != std::string::npos);

    {
      ConfigService reloaded;
      const auto catalog = reloaded.desktopWidgetSets();
      TEST_CHECK(catalog.sets.size() == 3);
      TEST_CHECK(catalog.sets[0].name == "Calm" && catalog.sets[0].widgets.size() == 2);
      TEST_CHECK(catalog.activeIdFor("DP-1") == catalog.sets[1].id);
      // Persisted sets are not a config layer: the widget snapshot itself is unaffected.
      TEST_CHECK(reloaded.config().desktopWidgets.widgets.empty());

      TEST_CHECK(reloaded.deleteDesktopWidgetSet(catalog.sets[0].id));
      const auto afterDelete = reloaded.desktopWidgetSets();
      TEST_CHECK(afterDelete.sets.size() == 2);
      TEST_CHECK(afterDelete.defaultIdFor(kWallpaperA, "DP-1") == catalog.sets[1].id);
      TEST_CHECK(afterDelete.activeIdFor("DP-1") == catalog.sets[1].id);
    }

    std::filesystem::remove_all(root);
  }

} // namespace

int main() {
  checkNormalize();
  checkCatalogRules();
  checkWidgetsForOutput();
  checkPersistence();
  return 0;
}
