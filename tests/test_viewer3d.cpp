#include "terrain_3d.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace dterrain::viewer3d;

namespace {

bool close(double a, double b, double tolerance = 1.0e-10) {
    return std::abs(a - b) <= tolerance;
}

void test_vector_math() {
    const Vec3 x{1.0, 0.0, 0.0};
    const Vec3 y{0.0, 1.0, 0.0};
    const Vec3 z = cross(x, y);
    assert(close(z.x, 0.0));
    assert(close(z.y, 0.0));
    assert(close(z.z, 1.0));
    assert(close(dot(x, y), 0.0));
    assert(close(length(normalized({3.0, 4.0, 0.0})), 1.0));
}

void test_center_projection() {
    Camera camera{};
    const Projection center = project(camera.target, camera, 16.0 / 9.0);
    assert(center.visible);
    assert(close(center.x, 0.0));
    assert(close(center.y, 0.0));
    assert(close(center.depth, camera.distance));
}

void test_orbit_and_limits() {
    Camera camera{};
    const double initial_distance = length(camera.position() - camera.target);
    camera.orbit(0.5, 10.0);
    assert(close(length(camera.position() - camera.target), initial_distance));
    assert(camera.pitch <= 85.0 * pi / 180.0);
    camera.orbit(0.0, -20.0);
    assert(camera.pitch >= 5.0 * pi / 180.0);
}

void test_navigation() {
    Camera camera{};
    const Vec3 initial = camera.target;
    pan(camera, 0.2, -0.1);
    assert(length(camera.target - initial) > 0.1);
    const Vec3 panned = camera.target;
    roam_xy(camera, 0.3, 0.0);
    assert(close(camera.target.z, panned.z));
    assert(length(camera.target - panned) > 0.29);
    camera.dolly(0.5);
    assert(close(camera.distance, 1.6));
    camera.dolly(0.0001);
    assert(close(camera.distance, 0.18));
}

void test_ray_triangle_and_chunking() {
    Camera camera{};
    const Ray ray = screen_ray(camera, 0.0, 0.0, 16.0 / 9.0);
    assert(close(length(ray.direction), 1.0));
    double distance = 0.0;
    Vec3 barycentric{};
    assert(intersect_triangle(
        ray, {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0}, {0.0, 1.0, 0.0},
        distance, &barycentric));
    assert(distance > 0.0);
    assert(close(barycentric.x + barycentric.y + barycentric.z, 1.0));

    const auto ranges = make_chunk_ranges(1001, 256);
    assert(ranges.size() == 4);
    assert(ranges.front().first_triangle == 0);
    assert(ranges.front().triangle_count == 256);
    assert(ranges.back().first_triangle == 768);
    assert(ranges.back().triangle_count == 233);
    assert(make_chunk_ranges(12, 0).empty());
}

void test_culling_and_terrain_follow() {
    Camera camera{};
    assert(sphere_in_view({camera.target, 0.1}, camera, 16.0 / 9.0));
    assert(!sphere_in_view({{100.0, 100.0, 100.0}, 0.1}, camera,
                           16.0 / 9.0));
    assert(close(terrain_follow_height(0.0, 0.4, 0.1, 1.0), 0.5));
    assert(close(terrain_follow_height(0.0, 0.4, 0.1, 0.5), 0.25));
}

} // namespace

int main() {
    test_vector_math();
    test_center_projection();
    test_orbit_and_limits();
    test_navigation();
    test_ray_triangle_and_chunking();
    test_culling_and_terrain_follow();
    std::cout << "viewer3d tests passed\n";
    return 0;
}
