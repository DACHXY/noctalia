// Locks the 3D tilt POC: the Mat3 homography must project like the shaders expect, keep an
// invertible transform for hit testing, bound the tilted surface correctly, and survive a config
// load plus an editor-driven settings write without losing the clamped values.

#include "config/config_service.h"
#include "config/config_types.h"
#include "render/core/mat3.h"
#include "shell/desktop/widget_transform.h"
#include "tests/test_check.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <unistd.h>

namespace {

  constexpr float kEpsilon = 0.01F;

  float deg(float degrees) { return degrees * std::numbers::pi_v<float> / 180.0F; }

  bool nearlyEqual(float lhs, float rhs, float epsilon = kEpsilon) { return std::abs(lhs - rhs) <= epsilon; }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void checkHomography() {
    // No tilt is exactly the identity, so every existing widget keeps the affine path.
    const Mat3 flat = Mat3::tiltProjection(0.0F, 0.0F, 1000.0F);
    TEST_CHECK(flat.isAffine());
    const Vec2 flatPoint = flat.transformPoint(120.0F, -40.0F);
    TEST_CHECK(nearlyEqual(flatPoint.x, 120.0F));
    TEST_CHECK(nearlyEqual(flatPoint.y, -40.0F));

    constexpr float distance = 1000.0F;
    const float tiltX = 0.6F;
    const float tiltY = -0.35F;
    const Mat3 tilt = Mat3::tiltProjection(tiltX, tiltY, distance);
    TEST_CHECK(!tilt.isAffine());

    // R = Ry(yaw) * Rx(pitch) applied to (x, y, 0), then screen = (d * X / (d + Z), d * Y / (d + Z)).
    const float x = 150.0F;
    const float y = -80.0F;
    const float expectedX = std::cos(tiltY) * x + std::sin(tiltY) * std::sin(tiltX) * y;
    const float expectedY = std::cos(tiltX) * y;
    const float expectedZ = std::cos(tiltY) * std::sin(tiltX) * y - std::sin(tiltY) * x;
    // A rigid rotation preserves length; a per-axis approximation would shear the card.
    TEST_CHECK(nearlyEqual(
        std::sqrt(expectedX * expectedX + expectedY * expectedY + expectedZ * expectedZ), std::sqrt(x * x + y * y)
    ));
    const Vec2 projected = tilt.transformPoint(x, y);
    TEST_CHECK(nearlyEqual(projected.x, distance * expectedX / (distance + expectedZ)));
    TEST_CHECK(nearlyEqual(projected.y, distance * expectedY / (distance + expectedZ)));
    // The centre sits on the w = 1 plane, so it must not move.
    const Vec2 center = tilt.transformPoint(0.0F, 0.0F);
    TEST_CHECK(nearlyEqual(center.x, 0.0F));
    TEST_CHECK(nearlyEqual(center.y, 0.0F));

    // An affine matrix still transforms without a perspective divide.
    const Mat3 affine = Mat3::translation(10.0F, 20.0F) * Mat3::rotation(0.5F) * Mat3::scale(2.0F, 3.0F);
    const Vec2 affinePoint = affine.transformPoint(4.0F, 5.0F);
    TEST_CHECK(nearlyEqual(affinePoint.x, 10.0F + std::cos(0.5F) * 8.0F - std::sin(0.5F) * 15.0F));
    TEST_CHECK(nearlyEqual(affinePoint.y, 20.0F + std::sin(0.5F) * 8.0F + std::cos(0.5F) * 15.0F));
  }

  void checkInverseRoundTrip() {
    // The renderer's own local -> output transform must invert exactly for pointer hit testing.
    const Mat3 transform = widgetContentTransform(640.0F, 360.0F, 400.0F, 240.0F, 1.0F, 0.4F, 0.6F, -0.35F);
    const Mat3 inverse = transform.inverse();
    for (const Vec2 local : {Vec2{0.0F, 0.0F}, Vec2{400.0F, 0.0F}, Vec2{400.0F, 240.0F}, Vec2{0.0F, 240.0F}}) {
      const Vec2 scene = transform.transformPoint(local.x, local.y);
      const Vec2 restored = inverse.transformPoint(scene.x, scene.y);
      TEST_CHECK(nearlyEqual(restored.x, local.x, 0.05F));
      TEST_CHECK(nearlyEqual(restored.y, local.y, 0.05F));
    }
    const Vec2 center = transform.transformPoint(200.0F, 120.0F);
    TEST_CHECK(nearlyEqual(center.x, 640.0F));
    TEST_CHECK(nearlyEqual(center.y, 360.0F));
  }

  void checkTiltedGeometry() {
    constexpr float width = 400.0F;
    constexpr float height = 240.0F;
    constexpr float cx = 640.0F;
    constexpr float cy = 360.0F;
    constexpr float tiltX = 0.6F;
    constexpr float tiltY = -0.35F;
    constexpr float outputWidth = 1920.0F;
    constexpr float outputHeight = 1080.0F;

    const auto flat = computeWidgetTransformBounds(cx, cy, width, height, 1.0F, 0.0F, 0.0F, 0.0F);
    TEST_CHECK(nearlyEqual(flat.aabbWidth, width));
    TEST_CHECK(nearlyEqual(flat.aabbHeight, height));

    const auto tilted = computeWidgetTransformBounds(cx, cy, width, height, 1.0F, 0.0F, tiltX, tiltY);
    // The perspective divide turns the silhouette into a trapezoid, so the bounding box is no
    // longer centred on the widget centre, but it must still contain it.
    TEST_CHECK(cx > tilted.left && cx < tilted.left + tilted.aabbWidth);
    TEST_CHECK(cy > tilted.top && cy < tilted.top + tilted.aabbHeight);
    TEST_CHECK(tilted.aabbWidth > 1.0F && tilted.aabbHeight > 1.0F);

    // Every projected corner must land inside the surface the host allocates for it.
    const auto geometry =
        computeClippedWidgetSurfaceGeometry(cx, cy, width, height, 1.0F, 0.0F, tiltX, tiltY, outputWidth, outputHeight);
    const Mat3 transform = widgetContentTransform(cx, cy, width, height, 1.0F, 0.0F, tiltX, tiltY);
    const float right = static_cast<float>(geometry.marginLeft) + static_cast<float>(geometry.surfaceWidth);
    const float bottom = static_cast<float>(geometry.marginTop) + static_cast<float>(geometry.surfaceHeight);
    for (const Vec2 local : {Vec2{0.0F, 0.0F}, Vec2{width, 0.0F}, Vec2{width, height}, Vec2{0.0F, height}}) {
      const Vec2 scene = transform.transformPoint(local.x, local.y);
      TEST_CHECK(scene.x >= static_cast<float>(geometry.marginLeft) - 0.5F);
      TEST_CHECK(scene.y >= static_cast<float>(geometry.marginTop) - 0.5F);
      TEST_CHECK(scene.x <= right + 0.5F);
      TEST_CHECK(scene.y <= bottom + 0.5F);
    }

    // Clamping uses the tilted AABB, so a widget pushed off the left edge comes back with that
    // footprint inside the output.
    const auto clamped =
        clampWidgetCenterToOutput(-500.0F, 200.0F, width, height, 1.0F, 0.0F, tiltX, tiltY, outputWidth, outputHeight);
    TEST_CHECK(clamped.cx > -500.0F);
    TEST_CHECK(clamped.cx - tilted.aabbWidth * 0.5F >= -0.5F);
    TEST_CHECK(clamped.cy - tilted.aabbHeight * 0.5F >= -0.5F);
    TEST_CHECK(clamped.cy + tilted.aabbHeight * 0.5F <= outputHeight + 0.5F);
  }

  void checkConfigClampAndPersistence(const std::filesystem::path& root) {
    writeFile(root / "config" / "noctalia" / "config.toml", R"(
[desktop_widgets.widget.tilted]
type   = "clock"
cx     = 400.0
cy     = 300.0
tilt_x = 9.9
tilt_y = -0.3

[lockscreen_widgets.widget.tilted]
type   = "clock"
cx     = 400.0
cy     = 300.0
tilt_x = -2.0
tilt_y = 0.25
)");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

    {
      ConfigService config;
      TEST_CHECK(config.config().desktopWidgets.widgets.size() == 1);
      const DesktopWidgetState& desktop = config.config().desktopWidgets.widgets.front();
      // Out-of-range values clamp to the 80 degree limit instead of reaching an edge-on/back-face case.
      TEST_CHECK(desktop.tiltX == kDesktopWidgetMaxTiltRad);
      TEST_CHECK(nearlyEqual(desktop.tiltY, -0.3F));

      TEST_CHECK(config.config().lockscreenWidgets.widgets.size() == 1);
      const DesktopWidgetState& lockscreen = config.config().lockscreenWidgets.widgets.front();
      TEST_CHECK(nearlyEqual(lockscreen.tiltX, -kDesktopWidgetMaxTiltRad));
      TEST_CHECK(nearlyEqual(lockscreen.tiltY, 0.25F));

      // Simulate an editor move: the settings write must keep the tilt values it does not edit.
      DesktopWidgetsConfig moved = config.config().desktopWidgets;
      moved.widgets.front().cx = 500.0;
      TEST_CHECK(config.setDesktopWidgetsState(moved));
    }

    const std::string settings = readFile(root / "state" / "noctalia" / "settings.toml");
    TEST_CHECK(settings.find("tilt_x") != std::string::npos);
    TEST_CHECK(settings.find("tilt_y") != std::string::npos);

    {
      ConfigService reloaded;
      TEST_CHECK(reloaded.config().desktopWidgets.widgets.size() == 1);
      const DesktopWidgetState& widget = reloaded.config().desktopWidgets.widgets.front();
      TEST_CHECK(nearlyEqual(widget.cx, 500.0F));
      TEST_CHECK(widget.tiltX == kDesktopWidgetMaxTiltRad);
      TEST_CHECK(nearlyEqual(widget.tiltY, -0.3F));
    }
  }

  void checkTiltDrag() {
    // Horizontal travel drives tiltY, vertical travel drives tiltX: 80 degrees per 400 px.
    const WidgetTiltAngles right = widgetTiltFromDrag(200.0F, 0.0F, 0.0F, 0.0F, 0.0F, false, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(right.tiltY, deg(40.0F), 0.001F));
    TEST_CHECK(nearlyEqual(right.tiltX, 0.0F, 0.001F));

    // Dragging down pushes the top edge away from the viewer, so tiltX goes negative.
    const WidgetTiltAngles down = widgetTiltFromDrag(0.0F, 100.0F, 0.0F, 0.0F, 0.0F, false, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(down.tiltX, deg(-20.0F), 0.001F));
    TEST_CHECK(nearlyEqual(down.tiltY, 0.0F, 0.001F));

    // The delta is measured in the widget's un-rotated frame, so at 90 degrees a horizontal drag
    // tilts around the widget's own X axis instead of the output's.
    const WidgetTiltAngles rotated =
        widgetTiltFromDrag(100.0F, 0.0F, deg(90.0F), 0.0F, 0.0F, false, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(rotated.tiltX, deg(20.0F), 0.001F));
    TEST_CHECK(nearlyEqual(rotated.tiltY, 0.0F, 0.001F));

    // The drag is relative to the tilt the drag started from.
    const WidgetTiltAngles fromStart =
        widgetTiltFromDrag(0.0F, 0.0F, 0.0F, deg(12.0F), deg(-7.0F), false, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(fromStart.tiltX, deg(12.0F), 0.001F));
    TEST_CHECK(nearlyEqual(fromStart.tiltY, deg(-7.0F), 0.001F));

    // Snapping rounds each axis to 0.5 degree steps; 22.2 degrees of tilt lands on 22.
    const WidgetTiltAngles snapped = widgetTiltFromDrag(111.0F, 0.0F, 0.0F, 0.0F, 0.0F, true, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(snapped.tiltY, deg(22.0F), 0.001F));
    const WidgetTiltAngles unsnapped =
        widgetTiltFromDrag(111.0F, 0.0F, 0.0F, 0.0F, 0.0F, false, kDesktopWidgetMaxTiltRad);
    TEST_CHECK(nearlyEqual(unsnapped.tiltY, deg(22.2F), 0.001F));

    // Clamped to the same +/-80 degrees the config parser enforces, with or without snapping.
    for (const bool snap : {false, true}) {
      const WidgetTiltAngles farRight =
          widgetTiltFromDrag(1000.0F, 0.0F, 0.0F, 0.0F, 0.0F, snap, kDesktopWidgetMaxTiltRad);
      TEST_CHECK(farRight.tiltY == kDesktopWidgetMaxTiltRad);
      const WidgetTiltAngles farDown =
          widgetTiltFromDrag(0.0F, -1000.0F, 0.0F, 0.0F, 0.0F, snap, kDesktopWidgetMaxTiltRad);
      TEST_CHECK(farDown.tiltX == kDesktopWidgetMaxTiltRad);
    }
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-widget-tilt-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  checkHomography();
  checkInverseRoundTrip();
  checkTiltedGeometry();
  checkTiltDrag();
  checkConfigClampAndPersistence(root);

  std::filesystem::remove_all(root);
  return 0;
}
