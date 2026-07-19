#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

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

inline Vec3 operator/(Vec3 value, double scale) {
    return {value.x / scale, value.y / scale, value.z / scale};
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

struct Ray {
    Vec3 origin{};
    Vec3 direction{};
};

/* normalized_x/y use the same [-1, 1] convention as project(). */
inline Ray screen_ray(const Camera& camera, double normalized_x,
                      double normalized_y, double aspect) {
    const Basis basis = camera_basis(camera);
    const double tangent = std::tan(camera.fov_y * 0.5);
    return {basis.position,
            normalized(basis.forward +
                       basis.right * (normalized_x * tangent * aspect) +
                       basis.up * (normalized_y * tangent))};
}

inline bool intersect_triangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c,
                               double& distance,
                               Vec3* barycentric = nullptr) {
    constexpr double epsilon = 1.0e-12;
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 p = cross(ray.direction, edge2);
    const double determinant = dot(edge1, p);
    if (std::abs(determinant) <= epsilon) return false;
    const double inverse = 1.0 / determinant;
    const Vec3 offset = ray.origin - a;
    const double u = dot(offset, p) * inverse;
    if (u < 0.0 || u > 1.0) return false;
    const Vec3 q = cross(offset, edge1);
    const double v = dot(ray.direction, q) * inverse;
    if (v < 0.0 || u + v > 1.0) return false;
    const double hit_distance = dot(edge2, q) * inverse;
    if (hit_distance <= epsilon) return false;
    distance = hit_distance;
    if (barycentric) *barycentric = {1.0 - u - v, u, v};
    return true;
}

struct Sphere {
    Vec3 center{};
    double radius = 0.0;
};

inline bool sphere_in_view(const Sphere& sphere, const Camera& camera,
                           double aspect, double near_plane = 0.02,
                           double far_plane = 80.0) {
    if (!(aspect > 0.0) || !(sphere.radius >= 0.0)) return false;
    const Basis basis = camera_basis(camera);
    const Vec3 relative = sphere.center - basis.position;
    const double depth = dot(relative, basis.forward);
    if (depth + sphere.radius < near_plane ||
        depth - sphere.radius > far_plane) return false;
    const double tangent_y = std::tan(camera.fov_y * 0.5);
    const double tangent_x = tangent_y * aspect;
    if (std::abs(dot(relative, basis.right)) >
        std::max(0.0, depth) * tangent_x + sphere.radius)
        return false;
    if (std::abs(dot(relative, basis.up)) >
        std::max(0.0, depth) * tangent_y + sphere.radius)
        return false;
    return true;
}

struct ChunkRange {
    size_t first_triangle = 0;
    size_t triangle_count = 0;
};

inline std::vector<ChunkRange> make_chunk_ranges(size_t triangle_count,
                                                  size_t chunk_triangles) {
    std::vector<ChunkRange> ranges;
    if (triangle_count == 0 || chunk_triangles == 0) return ranges;
    ranges.reserve((triangle_count + chunk_triangles - 1) / chunk_triangles);
    for (size_t first = 0; first < triangle_count;) {
        const size_t count = std::min(chunk_triangles,
                                      triangle_count - first);
        ranges.push_back({first, count});
        first += count;
    }
    return ranges;
}

inline double terrain_follow_height(double current_target_z,
                                    double surface_z,
                                    double eye_clearance,
                                    double smoothing = 1.0) {
    smoothing = std::clamp(smoothing, 0.0, 1.0);
    const double desired = surface_z + std::max(0.0, eye_clearance);
    return current_target_z + (desired - current_target_z) * smoothing;
}

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
