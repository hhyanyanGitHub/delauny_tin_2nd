#pragma once

#include <algorithm>
#include <cmath>

namespace dterrain::viewer3d {

constexpr double pi = 3.14159265358979323846;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 value, double scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

inline double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline double length(Vec3 value) {
    return std::sqrt(dot(value, value));
}

inline Vec3 normalized(Vec3 value) {
    const double magnitude = length(value);
    return magnitude > 1.0e-15 ? value * (1.0 / magnitude) : Vec3{};
}

struct Camera {
    Vec3 target{};
    double yaw = -0.75 * pi;
    double pitch = 35.0 * pi / 180.0;
    double distance = 3.2;
    double fov_y = 48.0 * pi / 180.0;

    Vec3 position() const {
        const double horizontal = std::cos(pitch) * distance;
        return target + Vec3{horizontal * std::cos(yaw),
                             horizontal * std::sin(yaw),
                             std::sin(pitch) * distance};
    }

    void orbit(double delta_yaw, double delta_pitch) {
        yaw += delta_yaw;
        if (yaw > pi) yaw -= 2.0 * pi;
        if (yaw < -pi) yaw += 2.0 * pi;
        pitch = std::clamp(pitch + delta_pitch,
                           5.0 * pi / 180.0,
                           85.0 * pi / 180.0);
    }

    void dolly(double factor) {
        distance = std::clamp(distance * factor, 0.18, 40.0);
    }
};

struct Basis {
    Vec3 position{};
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
};

inline Basis camera_basis(const Camera& camera) {
    Basis basis{};
    basis.position = camera.position();
    basis.forward = normalized(camera.target - basis.position);
    basis.right = normalized(cross(basis.forward, {0.0, 0.0, 1.0}));
    if (length(basis.right) < 1.0e-12) basis.right = {1.0, 0.0, 0.0};
    basis.up = normalized(cross(basis.right, basis.forward));
    return basis;
}

inline void pan(Camera& camera, double right, double up) {
    const Basis basis = camera_basis(camera);
    camera.target = camera.target + basis.right * right + basis.up * up;
}

inline void roam_xy(Camera& camera, double forward, double right) {
    Vec3 heading{-std::cos(camera.yaw), -std::sin(camera.yaw), 0.0};
    heading = normalized(heading);
    const Vec3 lateral{-heading.y, heading.x, 0.0};
    camera.target = camera.target + heading * forward + lateral * right;
}

struct Projection {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;
    bool visible = false;
};

inline Projection project(Vec3 world, const Camera& camera,
                          double aspect, double near_plane = 0.02) {
    const Basis basis = camera_basis(camera);
    const Vec3 relative = world - basis.position;
    const double depth = dot(relative, basis.forward);
    if (depth <= near_plane || aspect <= 0.0) return {0.0, 0.0, depth, false};
    const double tangent = std::tan(camera.fov_y * 0.5);
    if (tangent <= 0.0) return {0.0, 0.0, depth, false};
    return {dot(relative, basis.right) / (depth * tangent * aspect),
            dot(relative, basis.up) / (depth * tangent),
            depth, true};
}

} // namespace dterrain::viewer3d
