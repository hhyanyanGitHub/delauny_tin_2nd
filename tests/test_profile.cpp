#include "terrain_profile.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace {

bool close(double left, double right, double tolerance = 1.0e-10) {
    return std::abs(left - right) <= tolerance;
}

void test_line_points() {
    using namespace dterrain::profile;
    const auto points = line_points({10.0, 20.0}, {14.0, 28.0}, 3);
    assert(points.size() == 3);
    assert(close(points[0].x, 10.0) && close(points[0].y, 20.0));
    assert(close(points[1].x, 12.0) && close(points[1].y, 24.0));
    assert(close(points[2].x, 14.0) && close(points[2].y, 28.0));
    assert(line_points({}, {}, 1).empty());
}

void test_triangle_height() {
    using namespace dterrain::profile;
    double z = 0.0;
    assert(triangle_height({0.25, 0.25}, {0.0, 0.0}, 10.0,
                           {1.0, 0.0}, 20.0, {0.0, 1.0}, 30.0, z));
    assert(close(z, 17.5));
    assert(triangle_height({100000000.25, 100000000.25},
                           {100000000.0, 100000000.0}, 10.0,
                           {100000001.0, 100000000.0}, 20.0,
                           {100000000.0, 100000001.0}, 30.0, z));
    assert(close(z, 17.5));
    assert(!triangle_height({0.0, 0.0}, {0.0, 0.0}, 1.0,
                            {1.0, 1.0}, 2.0, {2.0, 2.0}, 3.0, z));
    assert(segment_height({2.5, 0.0}, {0.0, 0.0}, 10.0,
                          {10.0, 0.0}, 30.0, z));
    assert(close(z, 15.0));
    assert(segment_height({100000002.5, 100000000.0},
                          {100000000.0, 100000000.0}, 10.0,
                          {100000010.0, 100000000.0}, 30.0, z));
    assert(close(z, 15.0));
    assert(!segment_height({0.0, 0.0}, {1.0, 1.0}, 10.0,
                           {1.0, 1.0}, 20.0, z));
}

void test_grid_math() {
    using namespace dterrain::profile;
    const double transform[6] = {100.0, 2.0, 0.5, 200.0, -0.25, 3.0};
    Point point{100.0 + 4.0 * 2.0 + 5.0 * 0.5,
                200.0 + 4.0 * -0.25 + 5.0 * 3.0};
    double column = 0.0;
    double row = 0.0;
    assert(grid_coordinates(transform, point, column, row));
    assert(close(column, 4.0));
    assert(close(row, 5.0));
    assert(close(bilinear(10.0, 20.0, 30.0, 40.0, 0.25, 0.5), 22.5));
}

void test_statistics_and_gaps() {
    using namespace dterrain::profile;
    std::vector<Sample> samples{
        {0.0, 0.0, 0.0, 10.0, true},
        {5.0, 5.0, 0.0, 15.0, true},
        {10.0, 10.0, 0.0, std::numeric_limits<double>::quiet_NaN(), false},
        {15.0, 15.0, 0.0, 12.0, true},
        {20.0, 20.0, 0.0, 8.0, true},
    };
    const auto stats = summarize(samples);
    assert(stats.valid_count == 4);
    assert(close(stats.horizontal_distance, 20.0));
    assert(close(stats.sampled_distance, 10.0));
    assert(close(stats.minimum_z, 8.0));
    assert(close(stats.maximum_z, 15.0));
    assert(close(stats.elevation_difference, -2.0));
    assert(close(stats.ascent, 5.0));
    assert(close(stats.descent, 4.0));
    assert(close(stats.maximum_absolute_grade_percent, 100.0));
}

} // namespace

int main() {
    test_line_points();
    test_triangle_height();
    test_grid_math();
    test_statistics_and_gaps();
    return 0;
}
