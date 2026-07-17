#ifndef DT_SURFACE_ANALYSIS_HPP
#define DT_SURFACE_ANALYSIS_HPP

#include "dt_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dt {

inline dt_surface_analysis make_surface_analysis(
    const dt_point3& query, double z, double dz_dx, double dz_dy,
    const dt_point3* support_points, uint32_t support_count,
    uint32_t flags = 0) {
    if (!std::isfinite(query.x) || !std::isfinite(query.y) ||
        !std::isfinite(z) || !std::isfinite(dz_dx) ||
        !std::isfinite(dz_dy) || !support_points ||
        support_count < 3 || support_count > 4) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid surface analysis input");
    }
    dt_surface_analysis result{};
    result.struct_size = sizeof(result);
    result.flags = flags;
    result.point = {query.x, query.y, z};
    result.dz_dx = dz_dx;
    result.dz_dy = dz_dy;
    const double gradient = std::hypot(dz_dx, dz_dy);
    constexpr double radians_to_degrees =
        180.0 / 3.141592653589793238462643383279502884;
    result.slope_degrees = std::atan(gradient) * radians_to_degrees;
    const double normal_length = std::hypot(gradient, 1.0);
    result.normal_x = -dz_dx / normal_length;
    result.normal_y = -dz_dy / normal_length;
    result.normal_z = 1.0 / normal_length;
    const double flat_tolerance =
        std::numeric_limits<double>::epsilon() * 64.0;
    if (gradient <= flat_tolerance) {
        result.flags |= DT_SURFACE_ASPECT_UNDEFINED;
        result.aspect_degrees = 0.0;
    } else {
        double aspect = std::atan2(-dz_dx, -dz_dy) * radians_to_degrees;
        if (aspect < 0.0) aspect += 360.0;
        result.aspect_degrees = aspect;
    }
    result.support_point_count = support_count;
    std::copy(support_points, support_points + support_count,
              result.support_points);
    return result;
}

inline dt_surface_analysis analyze_triangle_surface(
    const dt_triangle3& triangle, const dt_point3& query,
    uint32_t flags = 0) {
    const auto& p0 = triangle.vertex[0].point;
    const auto& p1 = triangle.vertex[1].point;
    const auto& p2 = triangle.vertex[2].point;
    const double dx1 = p1.x - p0.x;
    const double dy1 = p1.y - p0.y;
    const double dz1 = p1.z - p0.z;
    const double dx2 = p2.x - p0.x;
    const double dy2 = p2.y - p0.y;
    const double dz2 = p2.z - p0.z;
    const double determinant = dx1 * dy2 - dx2 * dy1;
    if (!std::isfinite(determinant) || determinant == 0.0) {
        throw Exception(DT_E_INTERNAL, "degenerate surface triangle");
    }
    const double dz_dx = (dz1 * dy2 - dz2 * dy1) / determinant;
    const double dz_dy = (dx1 * dz2 - dx2 * dz1) / determinant;
    const double z = p0.z + dz_dx * (query.x - p0.x) +
                     dz_dy * (query.y - p0.y);
    const dt_point3 support[3] = {p0, p1, p2};
    return make_surface_analysis(query, z, dz_dx, dz_dy,
                                 support, 3, flags);
}

} // namespace dt

#endif
