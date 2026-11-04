#pragma once

#include "config/config_types.h"
#include "config/desktop_widget_set.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_services.h"
#include "shell/desktop/wallpaper_mask.h"
#include "ui/dialogs/layer_popup_host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class DesktopWidgetsEditor;
class DesktopWidgetsHost;
class LockscreenWidgetsController;
class IpcService;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct WallpaperChange;

using DesktopWidgetsSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsControllerServices {
  DesktopWidgetServices widgets;
  LockscreenWidgetsController* lockscreenWidgets = nullptr;
};

class DesktopWidgetsController {
public:
  DesktopWidgetsController();
  ~DesktopWidgetsController();

  DesktopWidgetsController(const DesktopWidgetsController&) = delete;
  DesktopWidgetsController& operator=(const DesktopWidgetsController&) = delete;

  void initialize(const DesktopWidgetsControllerServices& services);

  void registerIpc(IpcService& ipc);
  void onOutputChange();
  void onSecondTick();
  // Applies the widget sets the new wallpapers carry. Called from the shared wallpaper-change
  // callback, so every path (IPC, picker, automation, config reload) lands here.
  void onWallpaperChange(const std::vector<WallpaperChange>& changes);
  // True when a visible widget renders a value that changes every second.
  [[nodiscard]] bool wantsSecondTicks() const;
  void requestUpdate();
  void requestLayout();
  void requestRedraw();
  void setWallpaperMask(std::uint64_t ownerId, const std::string& outputName, std::optional<OutputWallpaperMask> mask);
  void clearWallpaperMasks(std::uint64_t ownerId);

  void enterEdit();
  void exitEdit();
  void toggleEdit();
  void setOnEnterEditCallback(std::function<void()> callback);
  void setOnExitEditCallback(std::function<void()> callback);

  /// Hides on-screen desktop widgets while another overlay editor (e.g. lockscreen layout) is active.
  void suppressDisplay();
  void unsuppressDisplay();

  /// Ephemeral, IPC-driven runtime visibility override layered on top of the saved
  /// `desktop_widgets.enabled` setting (in the spirit of the bar's bar-show/bar-hide/bar-toggle, but
  /// bidirectional). The override is never persisted and resets to FollowConfig on restart, leaving
  /// the user's saved preference untouched. ForceShown reveals widgets even when the setting is
  /// disabled -- this is what lets opt-in workflows (saved default off, revealed on demand by e.g. a
  /// peek-desktop keybind) work without rewriting settings.toml. Hiding tears down the widget
  /// instances (applyVisibility -> host->hide()), so it also stops their rendering/compute.
  enum class RuntimeVisibility : std::uint8_t { FollowConfig, ForceShown, ForceHidden };
  void setRuntimeVisibility(RuntimeVisibility visibility);
  void toggleRuntimeVisibility();
  [[nodiscard]] bool isEffectivelyVisible() const noexcept;

  [[nodiscard]] bool isEditing() const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;
  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  void loadSnapshotFromConfig();
  void saveSnapshotToConfig();
  void applyVisibility();
  void handleConfigReload();
  void normalizeSnapshot();
  void syncWallpaperMasks();
  void pruneWallpaperMasks();
  [[nodiscard]] bool runtimeWantsVisible() const noexcept;
  // Replaces `outputKey`'s widgets with the set's, rebinding them to that output.
  void applyWidgetSetForOutput(const std::string& outputKey, const std::string& setID);
  // Captures the live widgets of an output back into its active set (the automatic write-back).
  void syncActiveWidgetSet(const std::string& outputKey);
  void captureEditorSnapshot();
  [[nodiscard]] bool isShownOn(const DesktopWidgetState& widget, const std::string& outputKey) const;
  [[nodiscard]] std::vector<DesktopWidgetState> widgetsShownOn(const std::string& outputKey) const;
  // The wallpaper currently shown on an output, as a set key.
  [[nodiscard]] std::string wallpaperKeyForOutput(const std::string& outputKey) const;
  [[nodiscard]] std::string defaultOutputKey() const;
  // Splits `<name...> [connector]` into a set name and an output key (defaults to the first output).
  [[nodiscard]] std::string parseWidgetSetArgs(const std::string& args, std::string& name) const;
  // Id rather than pointer: the catalog is a copy local to the lookup.
  [[nodiscard]] std::string findWidgetSetIdForOutput(const std::string& name, const std::string& outputKey) const;
  bool saveWidgetSet(const std::string& name, const std::string& outputKey);
  void registerWidgetSetIpc(IpcService& ipc);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  LockscreenWidgetsController* m_lockscreenWidgets = nullptr;
  RenderContext* m_renderContext = nullptr;

  DesktopWidgetsSnapshot m_snapshot;
  desktop_widgets::PlacementMapper m_placementMapper;
  OutputWallpaperMaskMap m_wallpaperMasks;
  bool m_initialized = false;
  bool m_displaySuppressed = false;
  RuntimeVisibility m_runtimeVisibility = RuntimeVisibility::FollowConfig;
  // Last-seen saved desktop_widgets.enabled; an explicit transition clears the runtime override.
  bool m_lastEnabled = false;
  std::function<void()> m_onEnterEdit;
  std::function<void()> m_onExitEdit;
  std::unique_ptr<DesktopWidgetsHost> m_host;
  std::unique_ptr<DesktopWidgetsEditor> m_editor;
};
