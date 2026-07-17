#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace dterrain::profile {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Sample {
    double distance = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = std::numeric_limits<double>::quiet_NaN();
    bool valid = false;
};

struct Statistics {
    std::size_t valid_count = 0;
    double horizontal_distance = 0.0;
    double sampled_distance = 0.0;
    double start_z = std::numeric_limits<double>::quiet_NaN();
    double end_z = std::numeric_limits<double>::quiet_NaN();
    double elevation_difference = std::numeric_limits<double>::quiet_NaN();
    double minimum_z = std::numeric_limits<double>::quiet_NaN();
    double maximum_z = std::numeric_limits<double>::quiet_NaN();
    double ascent = 0.0;
    double descent = 0.0;
    double maximum_absolute_grade_percent = 0.0;
};

inline std::vector<Point> line_points(const Point& start, const Point& end,
                                      std::size_t count) {
    if (count < 2 || !std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y)) {
        return {};
    }
    std::vector<Point> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double t = static_cast<double>(index) /
                         static_cast<double>(count - 1);
        result.push_back({start.x + (end.x - start.x) * t,
                          start.y + (end.y - start.y) * t});
    }
    return result;
}

inline bool triangle_height(const Point& query, const Point& a, double az,
                            const Point& b, double bz, const Point& c,
                            double cz, double& output_z) {
    const double denominator = (b.y - c.y) * (a.x - c.x) +
                               (c.x - b.x) * (a.y - c.y);
    const double scale = std::max(
        {1.0, std::abs(b.x - a.x), std::abs(b.y - a.y),
         std::abs(c.x - a.x), std::abs(c.y - a.y),
         std::abs(c.x - b.x), std::abs(c.y - b.y)});
    if (!std::isfinite(denominator) ||
        std::abs(denominator) <=
            std::numeric_limits<double>::epsilon() * scale * scale * 16.0) {
        return false;
    }
    const double wa = ((b.y - c.y) * (query.x - c.x) +
                       (c.x - b.x) * (query.y - c.y)) / denominator;
    const double wb = ((c.y - a.y) * (query.x - c.x) +
                       (a.x - c.x) * (query.y - c.y)) / denominator;
    const double wc = 1.0 - wa - wb;
    output_z = wa * az + wb * bz + wc * cz;
    return std::isfinite(output_z);
}

inline bool segment_height(const Point& query, const Point& a, double az,
                           const Point& b, double bz, double& output_z) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double squared_length = dx * dx + dy * dy;
    const double scale = std::max({1.0, std::abs(dx), std::abs(dy)});
    if (!std::isfinite(squared_length) ||
        squared_length <= std::numeric_limits<double>::epsilon() *
                              scale * scale * 16.0) {
        return false;
    }
    const double t = ((query.x - a.x) * dx + (query.y - a.y) * dy) /
                     squared_length;
    output_z = az + (bz - az) * std::clamp(t, 0.0, 1.0);
    return std::isfinite(output_z);
}

inline bool grid_coordinates(const double transform[6], const Point& point,
                             double& column, double& row) {
    const double determinant = transform[1] * transform[5] -
                               transform[2] * transform[4];
    const double scale = std::max({1.0, std::abs(transform[1]),
                                   std::abs(transform[2]),
                                   std::abs(transform[4]),
                                   std::abs(transform[5])});
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <=
            std::numeric_limits<double>::epsilon() * scale * scale * 16.0) {
        return false;
    }
    const double dx = point.x - transform[0];
    const double dy = point.y - transform[3];
    column = (dx * transform[5] - dy * transform[2]) / determinant;
    row = (dy * transform[1] - dx * transform[4]) / determinant;
    return std::isfinite(column) && std::isfinite(row);
}

inline double bilinear(double z00, double z10, double z01, double z11,
                       double column_fraction, double row_fraction) {
    const double top = z00 + (z10 - z00) * column_fraction;
    const double bottom = z01 + (z11 - z01) * column_fraction;
    return top + (bottom - top) * row_fraction;
}

inline Statistics summarize(const std::vector<Sample>& samples) {
    Statistics result{};
    if (samples.empty()) return result;
    result.horizontal_distance = std::max(0.0, samples.back().distance -
                                                samples.front().distance);
    if (samples.front().valid && std::isfinite(samples.front().z))
        result.start_z = samples.front().z;
    if (samples.back().valid && std::isfinite(samples.back().z))
        result.end_z = samples.back().z;
    if (std::isfinite(result.start_z) && std::isfinite(result.end_z))
        result.elevation_difference = result.end_z - result.start_z;

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (const auto& sample : samples) {
        if (!sample.valid || !std::isfinite(sample.z)) continue;
        ++result.valid_count;
        minimum = std::min(minimum, sample.z);
        maximum = std::max(maximum, sample.z);
    }
    if (result.valid_count != 0) {
        result.minimum_z = minimum;
        result.maximum_z = maximum;
    }

    for (std::size_t index = 1; index < samples.size(); ++index) {
        const auto& previous = samples[index - 1];
        const auto& current = samples[index];
        if (!previous.valid || !current.valid ||
            !std::isfinite(previous.z) || !std::isfinite(current.z)) {
            continue;
        }
        const double distance = current.distance - previous.distance;
        if (!(distance > 0.0) || !std::isfinite(distance)) continue;
        const double difference = current.z - previous.z;
        if (difference >= 0.0) result.ascent += difference;
        else result.descent += -difference;
        result.sampled_distance += distance;
        result.maximum_absolute_grade_percent = std::max(
            result.maximum_absolute_grade_percent,
            std::abs(difference / distance) * 100.0);
    }
    return result;
}

} // namespace dterrain::profile
