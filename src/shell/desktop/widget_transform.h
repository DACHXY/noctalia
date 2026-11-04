#pragma once

#include "render/core/mat3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

struct WidgetTransformBounds {
  float scaledWidth = 0.0F;
  float scaledHeight = 0.0F;
  float aabbWidth = 0.0F;
  float aabbHeight = 0.0F;
  float left = 0.0F;
  float top = 0.0F;
};

struct WidgetTransformSurfaceGeometry {
  std::uint32_t surfaceWidth = 1;
  std::uint32_t surfaceHeight = 1;
  std::int32_t marginLeft = 0;
  std::int32_t marginTop = 0;
};

struct WidgetTransformClampResult {
  float cx = 0.0F;
  float cy = 0.0F;
};

inline constexpr float kDesktopWidgetMinVisibleFraction = 0.5F;

// Editor tilt drag: 80 degrees of tilt per 400 logical px of pointer travel, snapped to 5 degree
// steps when the editor's snap mode is on.
inline constexpr float kDesktopWidgetTiltRadPerPx = (80.0F * std::numbers::pi_v<float> / 180.0F) / 400.0F;
inline constexpr float kDesktopWidgetTiltSnapRad = 0.5F * std::numbers::pi_v<float> / 180.0F;

struct WidgetTiltAngles {
  float tiltX = 0.0F;
  float tiltY = 0.0F;
};

// Maps a pointer drag delta (output space, logical px) onto tilt angles. The delta is first
// brought into the widget's un-rotated frame, so the edge under the pointer follows the pointer
// at any rotation: horizontal travel drives tiltY and vertical travel drives tiltX. Positive
// tiltX brings the top edge towards the viewer, so dragging down (positive dy) pushes the top
// edge away and lowers tiltX; positive tiltY brings the right edge closer, so dragging right
// raises tiltY. Both axes are clamped to +/- maxTiltRad.
inline WidgetTiltAngles widgetTiltFromDrag(
    float deltaX, float deltaY, float rotationRad, float startTiltX, float startTiltY, bool snap, float maxTiltRad
) {
  const float cosR = std::cos(rotationRad);
  const float sinR = std::sin(rotationRad);
  const float localX = deltaX * cosR + deltaY * sinR;
  const float localY = -deltaX * sinR + deltaY * cosR;

  WidgetTiltAngles angles{
      .tiltX = startTiltX - localY * kDesktopWidgetTiltRadPerPx,
      .tiltY = startTiltY + localX * kDesktopWidgetTiltRadPerPx,
  };
  if (snap) {
    angles.tiltX = std::round(angles.tiltX / kDesktopWidgetTiltSnapRad) * kDesktopWidgetTiltSnapRad;
    angles.tiltY = std::round(angles.tiltY / kDesktopWidgetTiltSnapRad) * kDesktopWidgetTiltSnapRad;
  }
  angles.tiltX = std::clamp(angles.tiltX, -maxTiltRad, maxTiltRad);
  angles.tiltY = std::clamp(angles.tiltY, -maxTiltRad, maxTiltRad);
  return angles;
}

// Builds the same local -> output transform the renderer uses for a widget, so
// the bounds and the rendered surface agree for any tilt.
inline Mat3 widgetContentTransform(
    float cx, float cy, float width, float height, float scale, float rotationRad, float tiltX, float tiltY
) {
  const float clampedScale = std::max(0.01F, scale);
  return Mat3::translation(cx, cy)
      * Mat3::rotation(rotationRad)
      * Mat3::tiltProjection(tiltX, tiltY, tiltPerspectiveDistance(width, height))
      * Mat3::scale(clampedScale, clampedScale)
      * Mat3::translation(-width * 0.5F, -height * 0.5F);
}

inline WidgetTransformBounds computeWidgetTransformBounds(
    float cx, float cy, float width, float height, float scale, float rotationRad, float tiltX, float tiltY
) {
  WidgetTransformBounds bounds;
  const float clampedScale = std::max(0.01F, scale);
  const Mat3 transform = widgetContentTransform(cx, cy, width, height, scale, rotationRad, tiltX, tiltY);
  const std::array<Vec2, 4> corners = {
      transform.transformPoint(0.0F, 0.0F),
      transform.transformPoint(width, 0.0F),
      transform.transformPoint(width, height),
      transform.transformPoint(0.0F, height),
  };

  float minX = corners[0].x;
  float maxX = corners[0].x;
  float minY = corners[0].y;
  float maxY = corners[0].y;
  for (const Vec2& corner : corners) {
    minX = std::min(minX, corner.x);
    maxX = std::max(maxX, corner.x);
    minY = std::min(minY, corner.y);
    maxY = std::max(maxY, corner.y);
  }

  bounds.scaledWidth = std::max(1.0F, width * clampedScale);
  bounds.scaledHeight = std::max(1.0F, height * clampedScale);
  bounds.aabbWidth = std::max(1.0F, maxX - minX);
  bounds.aabbHeight = std::max(1.0F, maxY - minY);
  bounds.left = minX;
  bounds.top = minY;
  return bounds;
}

inline WidgetTransformSurfaceGeometry computeWidgetSurfaceGeometry(
    float cx, float cy, float width, float height, float scale, float rotationRad, float tiltX, float tiltY
) {
  const WidgetTransformBounds bounds =
      computeWidgetTransformBounds(cx, cy, width, height, scale, rotationRad, tiltX, tiltY);
  WidgetTransformSurfaceGeometry geometry;
  geometry.surfaceWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(bounds.aabbWidth)));
  geometry.surfaceHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(bounds.aabbHeight)));
  geometry.marginLeft = static_cast<std::int32_t>(std::lround(cx - static_cast<float>(geometry.surfaceWidth) * 0.5F));
  geometry.marginTop = static_cast<std::int32_t>(std::lround(cy - static_cast<float>(geometry.surfaceHeight) * 0.5F));
  return geometry;
}

struct WidgetTransformClippedGeometry {
  std::uint32_t surfaceWidth = 1;
  std::uint32_t surfaceHeight = 1;
  std::int32_t marginLeft = 0;
  std::int32_t marginTop = 0;
  float contentOffsetX = 0.0F;
  float contentOffsetY = 0.0F;
};

inline WidgetTransformClippedGeometry computeClippedWidgetSurfaceGeometry(
    float cx, float cy, float width, float height, float scale, float rotationRad, float tiltX, float tiltY,
    float outputWidth, float outputHeight
) {
  const WidgetTransformBounds bounds =
      computeWidgetTransformBounds(cx, cy, width, height, scale, rotationRad, tiltX, tiltY);

  // Use the projected AABB edges, not cx +/- aabb/2: a tilted widget is a trapezoid, so its
  // bounding box is not centred on the widget centre.
  const float desiredLeft = bounds.left;
  const float desiredTop = bounds.top;
  const float desiredRight = bounds.left + bounds.aabbWidth;
  const float desiredBottom = bounds.top + bounds.aabbHeight;

  const float clippedLeft = std::max(0.0F, desiredLeft);
  const float clippedTop = std::max(0.0F, desiredTop);
  const float clippedRight = std::min(outputWidth, desiredRight);
  const float clippedBottom = std::min(outputHeight, desiredBottom);

  // Snap outward so the off-centre trapezoid of a tilted widget never loses a pixel column.
  const float snappedLeft = std::floor(clippedLeft);
  const float snappedTop = std::floor(clippedTop);
  const float visibleWidth = std::max(1.0F, std::ceil(clippedRight) - snappedLeft);
  const float visibleHeight = std::max(1.0F, std::ceil(clippedBottom) - snappedTop);

  WidgetTransformClippedGeometry geometry;
  geometry.surfaceWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(visibleWidth));
  geometry.surfaceHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(visibleHeight));
  geometry.marginLeft = static_cast<std::int32_t>(snappedLeft);
  geometry.marginTop = static_cast<std::int32_t>(snappedTop);
  geometry.contentOffsetX = cx - static_cast<float>(geometry.marginLeft);
  geometry.contentOffsetY = cy - static_cast<float>(geometry.marginTop);
  return geometry;
}

inline WidgetTransformClampResult clampWidgetCenterToOutput(
    float cx, float cy, float width, float height, float scale, float rotationRad, float tiltX, float tiltY,
    float outputWidth, float outputHeight, float minVisibleFraction = 1.0F
) {
  const WidgetTransformBounds bounds =
      computeWidgetTransformBounds(0.0F, 0.0F, width, height, scale, rotationRad, tiltX, tiltY);
  // Offset of the AABB from the widget centre; zero for an affine transform, non-zero when the
  // perspective divide skews the trapezoid.
  const float aabbLeft = bounds.left;
  const float aabbTop = bounds.top;
  const float aabbRight = bounds.left + bounds.aabbWidth;
  const float aabbBottom = bounds.top + bounds.aabbHeight;
  const float visibleFraction = std::clamp(minVisibleFraction, 0.0F, 1.0F);
  const float minVisibleWidth = std::min(outputWidth, bounds.aabbWidth * visibleFraction);
  const float minVisibleHeight = std::min(outputHeight, bounds.aabbHeight * visibleFraction);

  WidgetTransformClampResult clamped{cx, cy};
  const float minCenterX = minVisibleWidth - aabbRight;
  const float maxCenterX = outputWidth - minVisibleWidth - aabbLeft;
  const float minCenterY = minVisibleHeight - aabbBottom;
  const float maxCenterY = outputHeight - minVisibleHeight - aabbTop;

  clamped.cx = std::clamp(cx, minCenterX, maxCenterX);
  clamped.cy = std::clamp(cy, minCenterY, maxCenterY);

  return clamped;
}
