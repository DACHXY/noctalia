#include "shell/desktop/desktop_widgets_controller.h"

#include "config/config_service.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widgets_host.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/desktop/editor/desktop_widgets_editor_types.h"
#include "shell/lockscreen/lockscreen_widgets_controller.h"
#include "shell/wallpaper/wallpaper.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace {

  constexpr std::string_view kDesktopWidgetIdPrefix = "desktop-widget-";
  constexpr Logger kLog("desktop-mask");

  void clampOpacitySetting(DesktopWidgetState& widget, const std::string& key, double fallback) {
    const auto it = widget.settings.find(key);
    if (it == widget.settings.end()) {
      return;
    }
    if (const auto* doubleValue = std::get_if<double>(&it->second)) {
      widget.settings.insert_or_assign(key, std::clamp(*doubleValue, 0.0, 1.0));
      return;
    }
    if (const auto* intValue = std::get_if<std::int64_t>(&it->second)) {
      widget.settings.insert_or_assign(key, std::clamp(static_cast<double>(*intValue), 0.0, 1.0));
      return;
    }
    widget.settings.insert_or_assign(key, fallback);
  }

  void normalizeDesktopWidgetSettings(DesktopWidgetState& widget) {
    clampOpacitySetting(widget, "background_opacity", 0.8);

    if (widget.type == "sticker" || widget.type == "label") {
      const auto opacityIt = widget.settings.find("opacity");
      if (opacityIt == widget.settings.end()) {
        widget.settings.insert_or_assign("opacity", 1.0);
        return;
      }
      if (const auto* doubleValue = std::get_if<double>(&opacityIt->second)) {
        widget.settings.insert_or_assign("opacity", std::clamp(*doubleValue, 0.0, 1.0));
        return;
      }
      if (const auto* intValue = std::get_if<std::int64_t>(&opacityIt->second)) {
        const double clamped = std::clamp(static_cast<double>(*intValue), 0.0, 1.0);
        widget.settings.insert_or_assign("opacity", clamped);
        return;
      }
      widget.settings.insert_or_assign("opacity", 1.0);
      return;
    }

    if (widget.type != "audio_visualizer") {
      return;
    }

    widget.settings.erase("aspect_ratio"); // setting removed; drop stale key
    widget.settings.erase("min_value");
  }

  bool parseDesktopWidgetCounter(std::string_view id, std::uint64_t& value) {
    if (!id.starts_with(kDesktopWidgetIdPrefix)) {
      return false;
    }

    const std::string_view suffix = id.substr(kDesktopWidgetIdPrefix.size());
    if (suffix.empty()) {
      return false;
    }

    value = 0;
    const auto* begin = suffix.data();
    const auto* end = suffix.data() + suffix.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    return ec == std::errc{} && ptr == end;
  }

  std::string makeDesktopWidgetId(std::uint64_t counter) { return std::format("desktop-widget-{:016x}", counter); }

} // namespace

DesktopWidgetsController::DesktopWidgetsController() = default;

DesktopWidgetsController::~DesktopWidgetsController() = default;

void DesktopWidgetsController::initialize(const DesktopWidgetsControllerServices& services) {
  m_wayland = &services.widgets.wayland;
  m_config = services.widgets.config;
  m_lockscreenWidgets = services.lockscreenWidgets;
  m_renderContext = services.widgets.renderContext;
  m_host = std::make_unique<DesktopWidgetsHost>();
  m_host->initialize(services.widgets);
  m_editor = std::make_unique<DesktopWidgetsEditor>(DesktopWidgetsEditorProfile::desktop());
  m_editor->initialize(services.widgets);
  m_editor->setExitRequestedCallback([this] { exitEdit(); });
  loadSnapshotFromConfig();
  const bool placementChanged = m_placementMapper.remapForOutputChange(m_wayland->outputs(), m_snapshot.widgets);
  m_initialized = true;
  if (m_config != nullptr) {
    m_lastEnabled = m_config->config().desktopWidgets.enabled;
  }
  if (placementChanged) {
    saveSnapshotToConfig();
  }
  applyVisibility();

  if (m_config != nullptr) {
    m_config->addReloadCallback([this] { handleConfigReload(); }, "desktop-widgets");
  }
}

void DesktopWidgetsController::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::desktopWidgetsEdit, [this](const std::string&) -> std::string {
    enterEdit();
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsExit, [this](const std::string&) -> std::string {
    exitEdit();
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsToggleEdit, [this](const std::string&) -> std::string {
    toggleEdit();
    return "ok\n";
  });

  // Ephemeral runtime show/hide override layered on top of the saved `desktop_widgets.enabled`
  // setting (bidirectional version of bar-show/bar-hide/bar-toggle). These never touch settings.toml
  // -- they flip a runtime override that resets on restart -- so a peek-desktop keybind can reveal
  // widgets on demand without rewriting the user's saved preference on every keypress. `show` is a
  // force-show: it reveals widgets even when the saved setting is disabled, so an opt-in workflow
  // (saved default off, revealed only on demand) works without persisting transient state.
  ipc.bind(noctalia::cli::msg::desktopWidgetsShow, [this](const std::string&) -> std::string {
    setRuntimeVisibility(RuntimeVisibility::ForceShown);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsHide, [this](const std::string&) -> std::string {
    setRuntimeVisibility(RuntimeVisibility::ForceHidden);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsToggle, [this](const std::string&) -> std::string {
    toggleRuntimeVisibility();
    return isEffectivelyVisible() ? "shown\n" : "hidden\n";
  });

  registerWidgetSetIpc(ipc);
}

void DesktopWidgetsController::registerWidgetSetIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::desktopWidgetsSetList, [this](const std::string& args) -> std::string {
    std::string name;
    const std::string outputKey = parseWidgetSetArgs(args, name);
    if (outputKey.empty()) {
      return "error: no output to list widget sets for\n";
    }
    const auto catalog = m_config->desktopWidgetSets();
    const auto views = catalog.viewsFor(wallpaperKeyForOutput(outputKey), outputKey);
    if (views.empty()) {
      return "error: no widget sets for this wallpaper\n";
    }
    std::string out;
    for (const auto& view : views) {
      out += view.name;
      if (view.isDefault) {
        out += " (default)";
      }
      if (view.isActive) {
        out += " (active)";
      }
      out += "\n";
    }
    return out;
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsSetSave, [this](const std::string& args) -> std::string {
    std::string name;
    const std::string outputKey = parseWidgetSetArgs(args, name);
    if (name.empty()) {
      return "error: set name required (desktop-widgets-set-save <name> [connector])\n";
    }
    if (outputKey.empty()) {
      return "error: no output to save widget sets for\n";
    }
    if (!saveWidgetSet(name, outputKey)) {
      return "error: could not save the widget set\n";
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsSetApply, [this](const std::string& args) -> std::string {
    std::string name;
    const std::string outputKey = parseWidgetSetArgs(args, name);
    if (name.empty()) {
      return "error: set name required (desktop-widgets-set-apply <name> [connector])\n";
    }
    const std::string setID = findWidgetSetIdForOutput(name, outputKey);
    if (setID.empty()) {
      return "error: no widget set with that name for this wallpaper\n";
    }
    applyWidgetSetForOutput(outputKey, setID);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsSetDefault, [this](const std::string& args) -> std::string {
    std::string name;
    const std::string outputKey = parseWidgetSetArgs(args, name);
    if (name.empty()) {
      return "error: set name required (desktop-widgets-set-default <name> [connector])\n";
    }
    const std::string setID = findWidgetSetIdForOutput(name, outputKey);
    if (setID.empty() || !m_config->setDefaultDesktopWidgetSet(setID)) {
      return "error: no widget set with that name for this wallpaper\n";
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::desktopWidgetsSetDelete, [this](const std::string& args) -> std::string {
    std::string name;
    const std::string outputKey = parseWidgetSetArgs(args, name);
    if (name.empty()) {
      return "error: set name required (desktop-widgets-set-delete <name> [connector])\n";
    }
    const std::string setID = findWidgetSetIdForOutput(name, outputKey);
    if (setID.empty() || !m_config->deleteDesktopWidgetSet(setID)) {
      return "error: no widget set with that name for this wallpaper\n";
    }
    return "ok\n";
  });
}

std::string DesktopWidgetsController::parseWidgetSetArgs(const std::string& args, std::string& name) const {
  const auto tokens = StringUtils::splitWhitespace(StringUtils::trim(args));
  if (tokens.empty()) {
    return defaultOutputKey();
  }

  std::string outputKey;
  std::size_t nameCount = tokens.size();
  // The connector is the last token only when it names an output, so set names may contain spaces.
  if (tokens.size() >= 2
      && m_wayland != nullptr
      && desktop_widgets::findOutputByKey(*m_wayland, tokens.back()) != nullptr) {
    outputKey = tokens.back();
    nameCount = tokens.size() - 1;
  }
  for (std::size_t i = 0; i < nameCount; ++i) {
    if (i != 0) {
      name += ' ';
    }
    name += tokens[i];
  }
  return outputKey.empty() ? defaultOutputKey() : outputKey;
}

std::string DesktopWidgetsController::defaultOutputKey() const {
  if (m_wayland == nullptr) {
    return {};
  }
  for (const auto& output : m_wayland->outputs()) {
    if (output.done && output.output != nullptr && output.hasUsableGeometry()) {
      return desktop_widgets::outputKey(output);
    }
  }
  return {};
}

std::string DesktopWidgetsController::wallpaperKeyForOutput(const std::string& outputKey) const {
  if (m_config == nullptr) {
    return {};
  }
  std::string path = m_config->getWallpaperPath(outputKey);
  if (path.empty()) {
    path = m_config->getDefaultWallpaperPath();
  }
  return desktop_widgets::normalizeWidgetSetWallpaperKey(path);
}

std::string
DesktopWidgetsController::findWidgetSetIdForOutput(const std::string& name, const std::string& outputKey) const {
  if (m_config == nullptr) {
    return {};
  }
  const auto catalog = m_config->desktopWidgetSets();
  const auto* set = catalog.findByName(wallpaperKeyForOutput(outputKey), outputKey, name);
  return set == nullptr ? std::string() : set->id;
}

bool DesktopWidgetsController::isShownOn(const DesktopWidgetState& widget, const std::string& outputKey) const {
  // Match the output the widget is actually drawn on: an empty or unplugged outputName falls back
  // to another output, so comparing the stored name would miss those widgets.
  if (m_wayland == nullptr) {
    return widget.outputName == outputKey;
  }
  const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, widget);
  return output != nullptr && desktop_widgets::outputKey(*output) == outputKey;
}

std::vector<DesktopWidgetState> DesktopWidgetsController::widgetsShownOn(const std::string& outputKey) const {
  std::vector<DesktopWidgetState> widgets;
  for (const auto& widget : m_snapshot.widgets) {
    if (isShownOn(widget, outputKey)) {
      widgets.push_back(widget);
    }
  }
  return widgets;
}

void DesktopWidgetsController::captureEditorSnapshot() {
  // Mid-edit the editor holds the freshest placement, not m_snapshot.
  if (isEditing() && m_editor != nullptr) {
    m_snapshot = m_editor->snapshot();
    normalizeSnapshot();
  }
}

bool DesktopWidgetsController::saveWidgetSet(const std::string& name, const std::string& outputKey) {
  if (m_config == nullptr) {
    return false;
  }
  captureEditorSnapshot();

  const std::string wallpaperKey = wallpaperKeyForOutput(outputKey);
  if (wallpaperKey.empty()) {
    return false;
  }

  desktop_widgets::WidgetSetCatalog catalog = m_config->desktopWidgetSets();
  desktop_widgets::DesktopWidgetSet set;
  // Saving under an existing name overwrites that set instead of creating a near-duplicate.
  if (const auto* existing = catalog.findByName(wallpaperKey, outputKey, name)) {
    set.id = existing->id;
  }
  set.name = name;
  set.wallpaper = wallpaperKey;
  set.outputName = outputKey;
  set.widgets = widgetsShownOn(outputKey);

  if (!m_config->saveDesktopWidgetSet(set)) {
    return false;
  }
  const auto saved = m_config->desktopWidgetSets();
  if (const auto* stored = saved.findByName(wallpaperKey, outputKey, name)) {
    (void)m_config->setActiveDesktopWidgetSet(outputKey, stored->id);
  }
  return true;
}

void DesktopWidgetsController::onWallpaperChange(const std::vector<WallpaperChange>& changes) {
  if (!m_initialized || m_config == nullptr) {
    return;
  }
  // Taken even when nothing changed: a name that did not land must not leak into the next change.
  const std::string requestedName = m_config->takePendingWidgetSetName();
  if (changes.empty()) {
    return;
  }

  for (const auto& change : changes) {
    if (change.connector.empty() || change.path.empty()) {
      continue;
    }
    const std::string wallpaperKey = desktop_widgets::normalizeWidgetSetWallpaperKey(change.path);
    const auto catalog = m_config->desktopWidgetSets();
    // A name given for all outputs only exists on some of them; the others take their default.
    const auto* set = desktop_widgets::selectSetForApply(catalog, wallpaperKey, change.connector, requestedName);
    if (set == nullptr && !requestedName.empty()) {
      set = desktop_widgets::selectSetForApply(catalog, wallpaperKey, change.connector, {});
    }
    // No sets for this wallpaper: the widgets the user has now stay untouched, but they stop
    // belonging to the previous wallpaper's set, so later edits must not write back into it.
    if (set == nullptr) {
      kLog.debug("wallpaper {} on {} has no widget set to apply", wallpaperKey, change.connector);
      captureEditorSnapshot();
      syncActiveWidgetSet(change.connector);
      (void)m_config->setActiveDesktopWidgetSet(change.connector, {});
      continue;
    }
    kLog.info("applying widget set \"{}\" to {}", set->name, change.connector);
    applyWidgetSetForOutput(change.connector, set->id);
  }
}

void DesktopWidgetsController::applyWidgetSetForOutput(const std::string& outputKey, const std::string& setID) {
  if (m_config == nullptr || setID.empty()) {
    return;
  }
  const auto catalog = m_config->desktopWidgetSets();
  const auto* set = catalog.find(setID);
  if (set == nullptr) {
    return;
  }

  captureEditorSnapshot();
  // Fold pending edits into the set being replaced before its widgets leave the screen.
  syncActiveWidgetSet(outputKey);

  std::vector<DesktopWidgetState> widgets;
  widgets.reserve(m_snapshot.widgets.size() + set->widgets.size());
  for (auto& widget : m_snapshot.widgets) {
    if (!isShownOn(widget, outputKey)) {
      widgets.push_back(std::move(widget));
    }
  }
  for (const auto& widget : set->widgets) {
    DesktopWidgetState rebound = widget;
    rebound.outputName = outputKey;
    widgets.push_back(std::move(rebound));
  }
  m_snapshot.widgets = std::move(widgets);
  // Sets saved from the same initial layout share widget ids; normalizeSnapshot renames later
  // duplicates so the two outputs keep independent widgets.
  normalizeSnapshot();
  // A set keeps the output size it was captured against, so scale its positions onto the output
  // it lands on (a set imported from another machine or a resized monitor).
  (void)m_placementMapper.remapForOutputChange(m_wayland->outputs(), m_snapshot.widgets);
  // Save the snapshot before any other override write: every commit reloads the config, and the
  // reload handler re-reads m_snapshot from it, which would drop the set just swapped in.
  const std::string appliedID = set->id;
  saveSnapshotToConfig();
  (void)m_config->setActiveDesktopWidgetSet(outputKey, appliedID);

  if (isEditing() && m_editor != nullptr) {
    m_editor->replaceSnapshot(m_snapshot);
    m_editor->requestLayout();
  } else if (m_host != nullptr) {
    m_host->show(m_snapshot);
  }
}

void DesktopWidgetsController::syncActiveWidgetSet(const std::string& outputKey) {
  if (m_config == nullptr) {
    return;
  }
  const auto catalog = m_config->desktopWidgetSets();
  // The active marker is cleared whenever the output's widgets stop belonging to a set (see
  // onWallpaperChange), so the marker alone says where edits go. Checking the wallpaper here would
  // miss the write-back on a wallpaper change, where the config already holds the new path.
  const auto* set = catalog.find(catalog.activeIdFor(outputKey));
  if (set == nullptr) {
    return;
  }
  desktop_widgets::DesktopWidgetSet updated = *set;
  updated.outputName = outputKey;
  updated.widgets = widgetsShownOn(outputKey);
  (void)m_config->saveDesktopWidgetSet(updated);
}

bool DesktopWidgetsController::runtimeWantsVisible() const noexcept {
  switch (m_runtimeVisibility) {
  case RuntimeVisibility::ForceShown:
    return true;
  case RuntimeVisibility::ForceHidden:
    return false;
  case RuntimeVisibility::FollowConfig:
    return m_config != nullptr && m_config->config().desktopWidgets.enabled;
  }
  return false;
}

void DesktopWidgetsController::setRuntimeVisibility(RuntimeVisibility visibility) {
  if (m_runtimeVisibility == visibility) {
    return;
  }
  m_runtimeVisibility = visibility;
  applyVisibility();
}

void DesktopWidgetsController::toggleRuntimeVisibility() {
  setRuntimeVisibility(isEffectivelyVisible() ? RuntimeVisibility::ForceHidden : RuntimeVisibility::ForceShown);
}

bool DesktopWidgetsController::isEffectivelyVisible() const noexcept {
  if (!m_initialized) {
    return false;
  }
  return runtimeWantsVisible() && !m_displaySuppressed && !isEditing();
}

void DesktopWidgetsController::setWallpaperMask(
    std::uint64_t ownerId, const std::string& outputName, std::optional<OutputWallpaperMask> mask
) {
  if (ownerId == 0 || outputName.empty()) {
    kLog.warn("rejected wallpaper mask with missing owner or output");
    return;
  }

  const auto existing = m_wallpaperMasks.find(outputName);
  if (!mask.has_value()) {
    if (existing != m_wallpaperMasks.end() && existing->second.ownerId == ownerId) {
      m_wallpaperMasks.erase(existing);
      syncWallpaperMasks();
    }
    return;
  }

  if (mask->path.empty() || mask->wallpaperPath.empty()) {
    kLog.warn("rejected incomplete wallpaper mask for {}", outputName);
    return;
  }
  if (existing != m_wallpaperMasks.end() && existing->second.ownerId != ownerId) {
    kLog.warn("output {} already has a wallpaper mask owner", outputName);
    return;
  }
  if (m_wayland == nullptr || desktop_widgets::findOutputByKey(*m_wayland, outputName) == nullptr) {
    kLog.warn("rejected wallpaper mask for unknown output {}", outputName);
    return;
  }
  if (m_config == nullptr || m_config->getWallpaperPath(outputName) != mask->wallpaperPath) {
    kLog.warn("rejected wallpaper mask for stale wallpaper on {}", outputName);
    return;
  }

  mask->ownerId = ownerId;
  m_wallpaperMasks.insert_or_assign(outputName, std::move(*mask));
  syncWallpaperMasks();
}

void DesktopWidgetsController::clearWallpaperMasks(std::uint64_t ownerId) {
  if (ownerId == 0) {
    return;
  }
  const auto removed =
      std::erase_if(m_wallpaperMasks, [ownerId](const auto& item) { return item.second.ownerId == ownerId; });
  if (removed != 0) {
    syncWallpaperMasks();
  }
}

void DesktopWidgetsController::syncWallpaperMasks() {
  if (m_host != nullptr) {
    m_host->setWallpaperMasks(m_wallpaperMasks);
  }
}

void DesktopWidgetsController::pruneWallpaperMasks() {
  if (m_config == nullptr) {
    m_wallpaperMasks.clear();
    syncWallpaperMasks();
    return;
  }
  const auto removed = std::erase_if(m_wallpaperMasks, [this](const auto& item) {
    return m_config->getWallpaperPath(item.first) != item.second.wallpaperPath;
  });
  if (removed != 0) {
    syncWallpaperMasks();
  }
}

void DesktopWidgetsController::onOutputChange() {
  if (!m_initialized) {
    return;
  }
  bool placementChanged = m_placementMapper.remapForOutputChange(m_wayland->outputs(), m_snapshot.widgets);
  normalizeSnapshot();
  placementChanged |= m_placementMapper.remapForOutputChange(m_wayland->outputs(), m_snapshot.widgets);
  if (placementChanged) {
    saveSnapshotToConfig();
  }
  pruneWallpaperMasks();
  if (isEditing()) {
    m_editor->onOutputChange();
  } else if (m_host != nullptr) {
    m_host->onOutputChange();
  }
}

void DesktopWidgetsController::onSecondTick() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->onSecondTick();
  } else if (m_host != nullptr) {
    m_host->onSecondTick();
  }
}

bool DesktopWidgetsController::wantsSecondTicks() const {
  if (!m_initialized) {
    return false;
  }
  // The editor owns the surfaces while it is open, and it mirrors the live
  // widget values, so keep second precision for its duration.
  if (isEditing()) {
    return true;
  }
  return m_host != nullptr && m_host->wantsSecondTicks();
}

void DesktopWidgetsController::requestUpdate() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestUpdate();
  } else if (m_host != nullptr) {
    m_host->requestUpdate();
  }
}

void DesktopWidgetsController::requestLayout() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestLayout();
  } else if (m_host != nullptr) {
    m_host->requestLayout();
  }
}

void DesktopWidgetsController::requestRedraw() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestRedraw();
  } else if (m_host != nullptr) {
    m_host->requestRedraw();
  }
}

void DesktopWidgetsController::enterEdit() {
  if (!m_initialized || m_editor == nullptr || m_host == nullptr || isEditing()) {
    return;
  }
  if (m_lockscreenWidgets != nullptr && m_lockscreenWidgets->isEditing()) {
    m_lockscreenWidgets->exitEdit();
  }
  if (m_config != nullptr && !m_config->config().desktopWidgets.enabled) {
    return;
  }
  if (m_onEnterEdit) {
    m_onEnterEdit();
  }
  // Open the editor before tearing down host widgets so the PipeWire spectrum
  // listener hand-off does not briefly drop to zero listeners (which resets the
  // stream and leaves a new editor instance with empty spectrum values).
  m_editor->open(m_snapshot);
  m_host->hide();
}

void DesktopWidgetsController::exitEdit() {
  if (!isEditing() || m_editor == nullptr) {
    return;
  }

  m_snapshot = m_editor->snapshot();
  normalizeSnapshot();
  m_placementMapper.rebaseForCurrentOutputs(m_wayland->outputs(), m_snapshot.widgets);
  m_host->show(m_snapshot);
  (void)m_editor->close();
  saveSnapshotToConfig();
  // The edit session ends: fold everything the user touched into the sets that were live. Walk the
  // active markers rather than the widgets so an output whose widgets were all deleted still syncs.
  for (const auto& [outputKey, setID] : m_config->desktopWidgetSets().active) {
    syncActiveWidgetSet(outputKey);
  }
  applyVisibility();
  if (m_onExitEdit) {
    m_onExitEdit();
  }
}

void DesktopWidgetsController::toggleEdit() {
  if (isEditing()) {
    exitEdit();
  } else {
    enterEdit();
  }
}

void DesktopWidgetsController::setOnEnterEditCallback(std::function<void()> callback) {
  m_onEnterEdit = std::move(callback);
}

void DesktopWidgetsController::setOnExitEditCallback(std::function<void()> callback) {
  m_onExitEdit = std::move(callback);
}

void DesktopWidgetsController::suppressDisplay() {
  m_displaySuppressed = true;
  if (isEditing()) {
    exitEdit();
  } else if (m_host != nullptr) {
    m_host->hide();
  }
}

void DesktopWidgetsController::unsuppressDisplay() {
  m_displaySuppressed = false;
  applyVisibility();
}

bool DesktopWidgetsController::isEditing() const noexcept { return m_editor != nullptr && m_editor->isOpen(); }

std::optional<LayerPopupParentContext>
DesktopWidgetsController::popupParentContextForSurface(wl_surface* surface) const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->popupParentContextForSurface(surface);
}

std::optional<LayerPopupParentContext> DesktopWidgetsController::fallbackPopupParentContext() const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->fallbackPopupParentContext();
}

bool DesktopWidgetsController::onPointerEvent(const PointerEvent& event) {
  if (isEditing() && m_editor != nullptr) {
    return m_editor->onPointerEvent(event);
  }
  if (m_host != nullptr) {
    return m_host->onPointerEvent(event);
  }
  return false;
}

void DesktopWidgetsController::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isEditing() || m_editor == nullptr) {
    return;
  }
  m_editor->onKeyboardEvent(event);
}

void DesktopWidgetsController::loadSnapshotFromConfig() {
  if (m_config == nullptr) {
    m_snapshot = DesktopWidgetsSnapshot{};
    return;
  }
  m_snapshot = m_config->config().desktopWidgets;
  normalizeSnapshot();
}

void DesktopWidgetsController::saveSnapshotToConfig() {
  if (m_config == nullptr) {
    return;
  }
  m_config->setDesktopWidgetsState(m_snapshot);
}

void DesktopWidgetsController::applyVisibility() {
  if (!m_initialized || m_host == nullptr || m_config == nullptr) {
    return;
  }

  // The runtime override resolves against the saved setting: ForceShown reveals widgets even when the
  // setting is disabled, ForceHidden suppresses them even when enabled, FollowConfig honors it.
  if (!runtimeWantsVisible()) {
    if (isEditing() && m_editor != nullptr) {
      m_snapshot = m_editor->close();
      saveSnapshotToConfig();
    }
    m_host->hide();
    return;
  }

  if (m_displaySuppressed || isEditing()) {
    m_host->hide();
    return;
  }

  m_host->show(m_snapshot);
}

void DesktopWidgetsController::handleConfigReload() {
  if (!m_initialized) {
    return;
  }

  // An explicit change to the saved enable toggle cancels any IPC runtime override, so the settings
  // UI takes back control. Gated on an actual transition: unrelated reloads leave the override intact.
  if (m_config != nullptr) {
    const bool enabled = m_config->config().desktopWidgets.enabled;
    if (enabled != m_lastEnabled) {
      m_lastEnabled = enabled;
      m_runtimeVisibility = RuntimeVisibility::FollowConfig;
    }
  }
  pruneWallpaperMasks();

  const bool calendarChanged = m_config != nullptr && m_config->lastChange().calendar;
  if (!isEditing()) {
    loadSnapshotFromConfig();
    if (m_host != nullptr) {
      m_host->rebuild(m_snapshot);
      if (calendarChanged) {
        m_host->requestLayout();
      }
      // Plugin-level settings live in [plugin_settings], outside the widget snapshot, so
      // the host's instance diff cannot see them change.
      if (m_config != nullptr && m_config->lastChange().plugins) {
        m_host->reloadPluginWidgets();
      }
    }
  } else if (calendarChanged && m_editor != nullptr) {
    m_editor->requestLayout();
  }
  applyVisibility();
}

void DesktopWidgetsController::normalizeSnapshot() {
  if (m_wayland == nullptr) {
    return;
  }

  std::uint64_t maxCounter = 0;
  for (const auto& widget : m_snapshot.widgets) {
    std::uint64_t counter = 0;
    if (parseDesktopWidgetCounter(widget.id, counter)) {
      maxCounter = std::max(maxCounter, counter);
    }
  }

  std::unordered_set<std::string> seenIds;
  for (auto& widget : m_snapshot.widgets) {
    normalizeDesktopWidgetSettings(widget);

    if (widget.id.empty() || seenIds.contains(widget.id)) {
      const std::uint64_t nextCounter =
          maxCounter == std::numeric_limits<std::uint64_t>::max() ? maxCounter : (maxCounter + 1);
      maxCounter = nextCounter;
      widget.id = makeDesktopWidgetId(nextCounter);
    }
    seenIds.insert(widget.id);

    if (widget.outputName.empty()) {
      const WaylandOutput* output = desktop_widgets::resolveEffectiveOutput(*m_wayland, widget.outputName);
      if (output != nullptr) {
        widget.outputName = desktop_widgets::outputKey(*output);
      }
      continue;
    }

    if (desktop_widgets::selectOutput(m_wayland->outputs(), widget.outputName).ambiguous) {
      kLog.warn(
          "widget {} output selector '{}' matches multiple monitors; use a unique description or serial", widget.id,
          widget.outputName
      );
    }
    // cx/cy clamping is owned by the editor (during drag) and the host (on widget creation and
    // prepareFrame). Both of those paths know the widget's actual intrinsic size; clamping here
    // with an estimate can push widgets that the editor had legitimately placed at the edge.
  }
}
