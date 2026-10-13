#pragma once

// Editor-owned math utilities.
//
// These definitions are the editor's own -- they carry no dependency on the
// runtime. They started as a mirror of the legacy runtime's math header and
// were reduced to exactly what editor code consumes; runtime DSP math
// (fade-curve evaluation, fast trig approximations) never belonged on the
// editor side and was deliberately not carried over.

#include <cmath>
#include <cstdint>
#include <string>

namespace HarkenEditor {

//==============================================================================
// Vec3 - 3D vector for spatial authoring values (positions, directions)
//==============================================================================
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& other) const { return Vec3(x + other.x, y + other.y, z + other.z); }
    constexpr Vec3 operator-(const Vec3& other) const { return Vec3(x - other.x, y - other.y, z - other.z); }
    constexpr Vec3 operator*(float scalar) const { return Vec3(x * scalar, y * scalar, z * scalar); }
    constexpr Vec3 operator/(float scalar) const { return Vec3(x / scalar, y / scalar, z / scalar); }

    constexpr Vec3& operator+=(const Vec3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    constexpr Vec3& operator*=(float scalar) { x *= scalar; y *= scalar; z *= scalar; return *this; }

    constexpr float dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }

    constexpr Vec3 cross(const Vec3& other) const
    {
        return Vec3(
            y * other.z - z * other.y,
            z * other.x - x * other.z,
            x * other.y - y * other.x
        );
    }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    constexpr float lengthSquared() const { return x * x + y * y + z * z; }

    Vec3 normalized() const
    {
        float len = length();
        if (len > 0.0001f)
            return *this / len;
        return Vec3(0, 0, 0);
    }

    float distanceTo(const Vec3& other) const
    {
        return (*this - other).length();
    }

    bool equals(const Vec3& other, float epsilon = 0.0001f) const
    {
        return std::abs(x - other.x) < epsilon &&
               std::abs(y - other.y) < epsilon &&
               std::abs(z - other.z) < epsilon;
    }
};

inline constexpr Vec3 operator*(float scalar, const Vec3& v) { return v * scalar; }

//==============================================================================
// Math utilities
//==============================================================================
namespace Math {

/// Linear interpolation
inline constexpr float lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

} // namespace Math

//==============================================================================
// FNV-1a string hash.
//
// The editor derives runtime object IDs from names with this hash; the values
// must therefore stay bit-identical to the runtime's FNV-1a (same offset
// basis 2166136261, same prime 16777619, 32-bit) or editor-assigned IDs stop
// matching what a running engine resolves.
//==============================================================================
inline uint32_t hashString(const char* str) noexcept
{
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 16777619u;
    }
    return hash;
}

inline uint32_t hashString(const std::string& str) noexcept
{
    return hashString(str.c_str());
}

} // namespace HarkenEditor
