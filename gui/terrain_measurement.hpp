#pragma once

#include "terrain_profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

namespace dterrain::measurement {

using Point = dterrain::profile::Point;
using Sampler = std::function<bool(const Point&, double&)>;

struct PolygonMetrics {
    double area = 0.0;
    double perimeter = 0.0;
};

struct Statistics {
    PolygonMetrics polygon{};
    double datum_z = 0.0;
    double valid_plan_area = 0.0;
    double surface_area = 0.0;
    double minimum_z = std::numeric_limits<double>::quiet_NaN();
    double maximum_z = std::numeric_limits<double>::quiet_NaN();
    double mean_z = std::numeric_limits<double>::quiet_NaN();
    double cut_volume = 0.0;
    double fill_volume = 0.0;
    double net_cut_volume = 0.0;
    std::size_t valid_micro_triangle_count = 0;
    std::size_t total_micro_triangle_count = 0;
};

inline double cross(const Point& a, const Point& b, const Point& c) {
    return (b.x - a.x) * (c.y - a.y) -
           (b.y - a.y) * (c.x - a.x);
}

inline double geometry_scale(const std::vector<Point>& polygon) {
    if (polygon.empty()) return 1.0;
    double xmin = polygon.front().x;
    double xmax = xmin;
    double ymin = polygon.front().y;
    double ymax = ymin;
    for (const auto& point : polygon) {
        xmin = std::min(xmin, point.x);
        xmax = std::max(xmax, point.x);
        ymin = std::min(ymin, point.y);
        ymax = std::max(ymax, point.y);
    }
    return std::max({1.0, xmax - xmin, ymax - ymin});
}

inline double signed_area(const std::vector<Point>& polygon) {
    if (polygon.size() < 3) return 0.0;
    const Point origin = polygon.front();
    double twice_area = 0.0;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& a = polygon[index];
        const auto& b = polygon[(index + 1) % polygon.size()];
        twice_area += (a.x - origin.x) * (b.y - origin.y) -
                      (a.y - origin.y) * (b.x - origin.x);
    }
    return twice_area * 0.5;
}

inline PolygonMetrics polygon_metrics(const std::vector<Point>& polygon) {
    PolygonMetrics result{};
    result.area = std::abs(signed_area(polygon));
    if (polygon.size() < 2) return result;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& a = polygon[index];
        const auto& b = polygon[(index + 1) % polygon.size()];
        result.perimeter += std::hypot(b.x - a.x, b.y - a.y);
    }
    return result;
}

inline bool point_on_segment(const Point& point, const Point& a, const Point& b,
                             double area_tolerance,
                             double length_tolerance) {
    if (std::abs(cross(a, b, point)) > area_tolerance) return false;
    return point.x >= std::min(a.x, b.x) - length_tolerance &&
           point.x <= std::max(a.x, b.x) + length_tolerance &&
           point.y >= std::min(a.y, b.y) - length_tolerance &&
           point.y <= std::max(a.y, b.y) + length_tolerance;
}

inline int orientation(double value, double tolerance) {
    if (value > tolerance) return 1;
    if (value < -tolerance) return -1;
    return 0;
}

inline bool segments_intersect(const Point& a, const Point& b,
                               const Point& c, const Point& d,
                               double area_tolerance,
                               double length_tolerance) {
    const double ab_c = cross(a, b, c);
    const double ab_d = cross(a, b, d);
    const double cd_a = cross(c, d, a);
    const double cd_b = cross(c, d, b);
    const int o1 = orientation(ab_c, area_tolerance);
    const int o2 = orientation(ab_d, area_tolerance);
    const int o3 = orientation(cd_a, area_tolerance);
    const int o4 = orientation(cd_b, area_tolerance);
    if (o1 * o2 < 0 && o3 * o4 < 0) return true;
    if (o1 == 0 && point_on_segment(c, a, b, area_tolerance,
                                    length_tolerance)) return true;
    if (o2 == 0 && point_on_segment(d, a, b, area_tolerance,
                                    length_tolerance)) return true;
    if (o3 == 0 && point_on_segment(a, c, d, area_tolerance,
                                    length_tolerance)) return true;
    if (o4 == 0 && point_on_segment(b, c, d, area_tolerance,
                                    length_tolerance)) return true;
    return false;
}

inline bool is_simple_polygon(const std::vector<Point>& polygon) {
    if (polygon.size() < 3) return false;
    for (const auto& point : polygon) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    }
    const double scale = geometry_scale(polygon);
    const double length_tolerance =
        std::numeric_limits<double>::epsilon() * scale * 64.0;
    const double area_tolerance = length_tolerance * scale;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& a = polygon[index];
        const auto& b = polygon[(index + 1) % polygon.size()];
        if (std::hypot(b.x - a.x, b.y - a.y) <= length_tolerance)
            return false;
    }
    if (std::abs(signed_area(polygon)) <= area_tolerance) return false;
    for (std::size_t first = 0; first < polygon.size(); ++first) {
        const std::size_t first_next = (first + 1) % polygon.size();
        for (std::size_t second = first + 1; second < polygon.size(); ++second) {
            const std::size_t second_next = (second + 1) % polygon.size();
            if (first == second || first_next == second ||
                second_next == first) {
                continue;
            }
            if (segments_intersect(polygon[first], polygon[first_next],
                                   polygon[second], polygon[second_next],
                                   area_tolerance, length_tolerance)) {
                return false;
            }
        }
    }
    return true;
}

inline bool point_in_triangle(const Point& point,
                              const std::array<Point, 3>& triangle,
                              double tolerance) {
    return cross(triangle[0], triangle[1], point) >= -tolerance &&
           cross(triangle[1], triangle[2], point) >= -tolerance &&
           cross(triangle[2], triangle[0], point) >= -tolerance;
}

inline bool triangulate_polygon(const std::vector<Point>& input,
                                std::vector<std::array<Point, 3>>& output) {
    output.clear();
    if (!is_simple_polygon(input)) return false;
    std::vector<Point> polygon = input;
    if (signed_area(polygon) < 0.0) std::reverse(polygon.begin(), polygon.end());
    std::vector<std::size_t> indices(polygon.size());
    std::iota(indices.begin(), indices.end(), 0);
    const double scale = geometry_scale(polygon);
    const double tolerance =
        std::numeric_limits<double>::epsilon() * scale * scale * 64.0;
    while (indices.size() > 3) {
        bool found_ear = false;
        for (std::size_t position = 0; position < indices.size(); ++position) {
            const std::size_t previous =
                indices[(position + indices.size() - 1) % indices.size()];
            const std::size_t current = indices[position];
            const std::size_t next = indices[(position + 1) % indices.size()];
            std::array<Point, 3> candidate{
                polygon[previous], polygon[current], polygon[next]};
            if (cross(candidate[0], candidate[1], candidate[2]) <= tolerance)
                continue;
            bool contains_vertex = false;
            for (const auto index : indices) {
                if (index == previous || index == current || index == next)
                    continue;
                if (point_in_triangle(polygon[index], candidate, tolerance)) {
                    contains_vertex = true;
                    break;
                }
            }
            if (contains_vertex) continue;
            output.push_back(candidate);
            indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(position));
            found_ear = true;
            break;
        }
        if (!found_ear) {
            output.clear();
            return false;
        }
    }
    output.push_back(
        {polygon[indices[0]], polygon[indices[1]], polygon[indices[2]]});
    return true;
}

struct Node {
    Point point{};
    double z = std::numeric_limits<double>::quiet_NaN();
    bool valid = false;
};

inline Statistics integrate_polygon(const std::vector<Point>& polygon,
                                    double datum_z,
                                    std::size_t target_micro_triangles,
                                    const Sampler& sampler) {
    Statistics result{};
    result.datum_z = datum_z;
    result.polygon = polygon_metrics(polygon);
    if (!std::isfinite(datum_z) || !sampler ||
        !(result.polygon.area > 0.0)) {
        return result;
    }
    std::vector<std::array<Point, 3>> ears;
    if (!triangulate_polygon(polygon, ears)) return result;
    target_micro_triangles =
        std::clamp<std::size_t>(target_micro_triangles, 1, 200000);
    double weighted_z = 0.0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();

    auto accumulate = [&](const Node& input_a, const Node& input_b,
                          const Node& input_c) {
        ++result.total_micro_triangle_count;
        const Point centroid{(input_a.point.x + input_b.point.x +
                              input_c.point.x) / 3.0,
                             (input_a.point.y + input_b.point.y +
                              input_c.point.y) / 3.0};
        double centroid_z = 0.0;
        if (!sampler(centroid, centroid_z) || !std::isfinite(centroid_z))
            return;
        const double plan_area =
            std::abs(cross(input_a.point, input_b.point, input_c.point)) * 0.5;
        if (!(plan_area > 0.0)) return;
        const double za = input_a.valid ? input_a.z : centroid_z;
        const double zb = input_b.valid ? input_b.z : centroid_z;
        const double zc = input_c.valid ? input_c.z : centroid_z;
        const double mean_z = (za + zb + zc) / 3.0;
        const double abx = input_b.point.x - input_a.point.x;
        const double aby = input_b.point.y - input_a.point.y;
        const double abz = zb - za;
        const double acx = input_c.point.x - input_a.point.x;
        const double acy = input_c.point.y - input_a.point.y;
        const double acz = zc - za;
        const double cross_x = aby * acz - abz * acy;
        const double cross_y = abz * acx - abx * acz;
        const double cross_z = abx * acy - aby * acx;
        result.valid_plan_area += plan_area;
        result.surface_area +=
            0.5 * std::sqrt(cross_x * cross_x + cross_y * cross_y +
                            cross_z * cross_z);
        weighted_z += mean_z * plan_area;
        minimum = std::min({minimum, za, zb, zc, centroid_z});
        maximum = std::max({maximum, za, zb, zc, centroid_z});
        const double signed_volume = (mean_z - datum_z) * plan_area;
        if (signed_volume >= 0.0) result.cut_volume += signed_volume;
        else result.fill_volume += -signed_volume;
        ++result.valid_micro_triangle_count;
    };

    for (const auto& ear : ears) {
        const double ear_area = std::abs(cross(ear[0], ear[1], ear[2])) * 0.5;
        const double share = ear_area / result.polygon.area;
        const std::size_t divisions = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::ceil(
                   std::sqrt(share * target_micro_triangles))));
        std::vector<std::vector<Node>> rows(divisions + 1);
        for (std::size_t i = 0; i <= divisions; ++i) {
            rows[i].resize(divisions - i + 1);
            for (std::size_t j = 0; j + i <= divisions; ++j) {
                const double u = static_cast<double>(i) / divisions;
                const double v = static_cast<double>(j) / divisions;
                auto& node = rows[i][j];
                node.point = {
                    ear[0].x + (ear[1].x - ear[0].x) * u +
                        (ear[2].x - ear[0].x) * v,
                    ear[0].y + (ear[1].y - ear[0].y) * u +
                        (ear[2].y - ear[0].y) * v};
                node.valid = sampler(node.point, node.z) &&
                             std::isfinite(node.z);
            }
        }
        for (std::size_t i = 0; i < divisions; ++i) {
            for (std::size_t j = 0; i + j < divisions; ++j) {
                accumulate(rows[i][j], rows[i + 1][j], rows[i][j + 1]);
                if (i + j + 1 < divisions) {
                    accumulate(rows[i + 1][j], rows[i + 1][j + 1],
                               rows[i][j + 1]);
                }
            }
        }
    }
    if (result.valid_plan_area > 0.0) {
        result.minimum_z = minimum;
        result.maximum_z = maximum;
        result.mean_z = weighted_z / result.valid_plan_area;
    }
    result.net_cut_volume = result.cut_volume - result.fill_volume;
    return result;
}

} // namespace dterrain::measurement
