#include "terrain_measurement.hpp"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

bool close(double left, double right, double tolerance = 1.0e-9) {
    return std::abs(left - right) <= tolerance;
}

void test_polygon_validation_and_triangulation() {
    using namespace dterrain::measurement;
    const std::vector<Point> concave{
        {0.0, 0.0}, {2.0, 0.0}, {2.0, 1.0},
        {1.0, 0.5}, {0.0, 1.0}};
    assert(is_simple_polygon(concave));
    const auto metrics = polygon_metrics(concave);
    assert(close(metrics.area, 1.5));
    std::vector<std::array<Point, 3>> triangles;
    assert(triangulate_polygon(concave, triangles));
    assert(triangles.size() == concave.size() - 2);
    double area = 0.0;
    for (const auto& triangle : triangles)
        area += std::abs(cross(triangle[0], triangle[1], triangle[2])) * 0.5;
    assert(close(area, metrics.area));

    std::vector<Point> shifted = concave;
    for (auto& point : shifted) {
        point.x += 100000000.0;
        point.y += 200000000.0;
    }
    assert(is_simple_polygon(shifted));
    assert(close(polygon_metrics(shifted).area, metrics.area));
    assert(triangulate_polygon(shifted, triangles));

    const std::vector<Point> bow_tie{
        {0.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {1.0, 0.0}};
    assert(!is_simple_polygon(bow_tie));
    assert(!triangulate_polygon(bow_tie, triangles));
}

void test_planar_surface_and_volumes() {
    using namespace dterrain::measurement;
    const std::vector<Point> square{
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    const Sampler plane = [](const Point& point, double& z) {
        z = point.x + point.y;
        return true;
    };
    const auto above = integrate_polygon(square, 0.0, 100, plane);
    assert(close(above.polygon.area, 1.0));
    assert(close(above.polygon.perimeter, 4.0));
    assert(close(above.valid_plan_area, 1.0));
    assert(close(above.surface_area, std::sqrt(3.0), 1.0e-8));
    assert(close(above.mean_z, 1.0));
    assert(close(above.minimum_z, 0.0));
    assert(close(above.maximum_z, 2.0));
    assert(close(above.cut_volume, 1.0));
    assert(close(above.fill_volume, 0.0));
    assert(close(above.net_cut_volume, 1.0));

    const auto below = integrate_polygon(square, 2.0, 100, plane);
    assert(close(below.cut_volume, 0.0));
    assert(close(below.fill_volume, 1.0));
    assert(close(below.net_cut_volume, -1.0));
}

void test_invalid_domain_coverage() {
    using namespace dterrain::measurement;
    const std::vector<Point> square{
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    const Sampler half_domain = [](const Point& point, double& z) {
        if (point.x > 0.5) return false;
        z = 10.0;
        return true;
    };
    const auto stats = integrate_polygon(square, 0.0, 400, half_domain);
    assert(stats.valid_plan_area > 0.45 && stats.valid_plan_area < 0.55);
    assert(close(stats.mean_z, 10.0));
    assert(std::abs(stats.cut_volume - stats.valid_plan_area * 10.0) < 1.0e-8);
    const auto empty = integrate_polygon(
        square, 0.0, 20,
        [](const Point&, double&) { return false; });
    assert(close(empty.valid_plan_area, 0.0));
    assert(empty.valid_micro_triangle_count == 0);
}

} // namespace

int main() {
    test_polygon_validation_and_triangulation();
    test_planar_surface_and_volumes();
    test_invalid_domain_coverage();
    return 0;
}
