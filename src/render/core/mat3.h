#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Minimum perspective distance (logical px) used by tilt. The distance is
// derived from the node size so small and large widgets get a similar amount
// of perspective distortion.
inline constexpr float kTiltMinPerspectiveDistance = 1000.0F;

inline float tiltPerspectiveDistance(float width, float height) {
  return std::max(kTiltMinPerspectiveDistance, 2.0F * std::max(width, height));
}

struct Vec2 {
  float x = 0.0F;
  float y = 0.0F;
};

struct Mat3 {
  // Column-major for direct upload to GLSL mat3.
  std::array<float, 9> m = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};

  [[nodiscard]] static Mat3 identity() { return {}; }

  [[nodiscard]] static Mat3 translation(float tx, float ty) {
    Mat3 out;
    out.m = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, tx, ty, 1.0F};
    return out;
  }

  [[nodiscard]] static Mat3 rotation(float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    Mat3 out;
    out.m = {cs, sn, 0.0F, -sn, cs, 0.0F, 0.0F, 0.0F, 1.0F};
    return out;
  }

  [[nodiscard]] static Mat3 scale(float sx, float sy) {
    Mat3 out;
    out.m = {sx, 0.0F, 0.0F, 0.0F, sy, 0.0F, 0.0F, 0.0F, 1.0F};
    return out;
  }

  // Projection of a plane rotated by tiltX around the X axis and then tiltY
  // around the Y axis (R = Ry * Rx), seen from a camera at distance along +Z.
  // Positive tiltX brings the top edge closer to the viewer, positive tiltY
  // brings the right edge closer. The translation row is divided by the
  // distance so the node centre stays at w = 1.
  [[nodiscard]] static Mat3 tiltProjection(float tiltX, float tiltY, float distance) {
    const float cx = std::cos(tiltX);
    const float sx = std::sin(tiltX);
    const float cy = std::cos(tiltY);
    const float sy = std::sin(tiltY);
    const float invDistance = 1.0F / std::max(distance, 1.0F);
    Mat3 out;
    out.m = {cy, 0.0F, -sy * invDistance, sy * sx, cx, cy * sx * invDistance, 0.0F, 0.0F, 1.0F};
    return out;
  }

  [[nodiscard]] bool isAffine() const { return m[2] == 0.0F && m[5] == 0.0F && m[8] == 1.0F; }

  [[nodiscard]] Vec2 transformPoint(float x, float y) const {
    const Vec2 affine{
        .x = m[0] * x + m[3] * y + m[6],
        .y = m[1] * x + m[4] * y + m[7],
    };
    // Tilted (projective) transforms need the homogeneous divide; the projection keeps w == 1 at
    // the node centre, so affine matrices take the cheap branch and never pay for a division.
    const float w = m[2] * x + m[5] * y + m[8];
    if (w == 1.0F) {
      return affine;
    }
    const float invW = w != 0.0F ? 1.0F / w : 1.0F;
    return {.x = affine.x * invW, .y = affine.y * invW};
  }

  [[nodiscard]] Mat3 operator*(const Mat3& other) const {
    Mat3 out;
    for (std::size_t col = 0; col < 3; ++col) {
      for (std::size_t row = 0; row < 3; ++row) {
        out.m[col * 3 + row] = m[0 * 3 + row] * other.m[col * 3 + 0]
            + m[1 * 3 + row] * other.m[col * 3 + 1]
            + m[2 * 3 + row] * other.m[col * 3 + 2];
      }
    }
    return out;
  }

  [[nodiscard]] float determinant() const {
    return m[0] * (m[4] * m[8] - m[7] * m[5]) - m[3] * (m[1] * m[8] - m[7] * m[2]) + m[6] * (m[1] * m[5] - m[4] * m[2]);
  }

  [[nodiscard]] Mat3 inverse() const {
    const float det = determinant();
    if (std::abs(det) <= 0.000001F) {
      return identity();
    }

    const float invDet = 1.0F / det;
    Mat3 out;
    out.m = {
        (m[4] * m[8] - m[7] * m[5]) * invDet, (m[7] * m[2] - m[1] * m[8]) * invDet,
        (m[1] * m[5] - m[4] * m[2]) * invDet, (m[6] * m[5] - m[3] * m[8]) * invDet,
        (m[0] * m[8] - m[6] * m[2]) * invDet, (m[3] * m[2] - m[0] * m[5]) * invDet,
        (m[3] * m[7] - m[6] * m[4]) * invDet, (m[6] * m[1] - m[0] * m[7]) * invDet,
        (m[0] * m[4] - m[3] * m[1]) * invDet,
    };
    return out;
  }
};
