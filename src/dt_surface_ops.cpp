#include "dt_surface_ops.hpp"

#include <boost/geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dt {
namespace {

namespace bg = boost::geometry;
using Point2 = bg::model::d2::point_xy<double>;
using Polygon2 = bg::model::polygon<Point2, false, true>;

bool finite(double value) { return std::isfinite(value) != 0; }

Polygon2 copy_polygon(const dt_polygon_rings& input) {
    if (input.struct_size != 0 && input.struct_size < sizeof(input))
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "dt_polygon_rings struct_size is invalid");
    if (!input.points || !input.ring_offsets || input.ring_count == 0 ||
        input.point_count < 3 || input.ring_offsets[0] != 0 ||
        input.ring_count > input.point_count / 3 ||
        input.ring_offsets[input.ring_count] != input.point_count)
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid polygon ring arrays");
    Polygon2 polygon;
    polygon.inners().resize(static_cast<size_t>(input.ring_count - 1));
    for (uint64_t ring_index = 0; ring_index < input.ring_count; ++ring_index) {
        const uint64_t begin = input.ring_offsets[ring_index];
        const uint64_t end = input.ring_offsets[ring_index + 1];
        if (end < begin || end - begin < 3 || end > input.point_count)
            throw Exception(DT_E_INVALID_ARGUMENT, "invalid polygon ring offset");
        auto& ring = ring_index == 0
            ? polygon.outer()
            : polygon.inners()[static_cast<size_t>(ring_index - 1)];
        ring.reserve(static_cast<size_t>(end - begin + 1));
        for (uint64_t index = begin; index < end; ++index) {
            const auto& point = input.points[index];
            if (!finite(point.x) || !finite(point.y))
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "polygon coordinates must be finite");
            ring.emplace_back(point.x, point.y);
        }
        if (bg::equals(ring.front(), ring.back()))
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "polygon rings must not repeat the first point");
        ring.push_back(ring.front());
    }
    bg::correct(polygon);
    std::string reason;
    if (!bg::is_valid(polygon, reason))
        throw Exception(DT_E_INVALID_ARGUMENT,
                        std::string("invalid polygon: ") + reason);
    return polygon;
}

double interpolate_triangle_z(const dt_triangle3& triangle, double x, double y) {
    const auto& a = triangle.vertex[0].point;
    const auto& b = triangle.vertex[1].point;
    const auto& c = triangle.vertex[2].point;
    const double denominator =
        (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (denominator == 0.0)
        throw Exception(DT_E_INTERNAL, "degenerate source triangle");
    const double wa = ((b.y - c.y) * (x - c.x) +
                       (c.x - b.x) * (y - c.y)) / denominator;
    const double wb = ((c.y - a.y) * (x - c.x) +
                       (a.x - c.x) * (y - c.y)) / denominator;
    return wa * a.z + wb * b.z + (1.0 - wa - wb) * c.z;
}

template <class Visit>
std::unique_ptr<SurfaceClipData> clip_surface(
    const Polygon2& clip, uint64_t generation, Visit&& visit) {
    auto result = std::make_unique<SurfaceClipData>();
    result->generation = generation;
    visit([&](const dt_triangle3& triangle) {
        const uint64_t source_index = result->source_triangle_count++;
        Polygon2 source;
        for (const auto& vertex : triangle.vertex)
            source.outer().emplace_back(vertex.point.x, vertex.point.y);
        source.outer().push_back(source.outer().front());
        bg::correct(source);
        std::vector<Polygon2> pieces;
        bg::intersection(source, clip, pieces);
        for (auto& polygon : pieces) {
            bg::correct(polygon);
            const double area = std::abs(bg::area(polygon));
            if (!(area > 0.0)) continue;
            dt_surface_clip_piece piece{};
            piece.source_triangle_index = source_index;
            piece.first_ring = result->rings.size();
            auto append_ring = [&](const auto& ring, bool hole) {
                if (ring.size() < 4) return;
                dt_surface_clip_ring output{};
                output.first_point = result->points.size();
                output.point_count = ring.size() - 1;
                output.is_hole = hole ? 1u : 0u;
                for (size_t i = 0; i + 1 < ring.size(); ++i) {
                    const double x = bg::get<0>(ring[i]);
                    const double y = bg::get<1>(ring[i]);
                    result->points.push_back(
                        {x, y, interpolate_triangle_z(triangle, x, y)});
                }
                result->rings.push_back(output);
                ++piece.ring_count;
            };
            append_ring(polygon.outer(), false);
            for (const auto& inner : polygon.inners()) append_ring(inner, true);
            if (piece.ring_count != 0) {
                result->pieces.push_back(piece);
                result->exact_plan_area += area;
            }
        }
    });
    return result;
}

double halton(uint64_t index, uint32_t base) {
    double value = 0.0;
    double factor = 1.0;
    while (index != 0) {
        factor /= static_cast<double>(base);
        value += factor * static_cast<double>(index % base);
        index /= base;
    }
    return value;
}

struct SamplePoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

bool sample_grid(const Grid& grid, double x, double y, double& z) {
    try {
        z = grid.sample_height_xy({x, y, 0.0});
        return finite(z);
    } catch (const Exception& error) {
        if (error.status() == DT_E_NOT_FOUND) return false;
        throw;
    }
}

void require_compatible_crs(const Grid& a, const Grid& b) {
    if (a.crs_wkt() != b.crs_wkt())
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "surface CRS metadata does not match");
}

double grid_pixel_scale(const Grid& grid) {
    const auto* gt = grid.transform();
    const double area = std::abs(gt[1] * gt[5] - gt[2] * gt[4]);
    if (!(area > 0.0) || !finite(area))
        throw Exception(DT_E_INVALID_ARGUMENT, "GRID affine is singular");
    return std::sqrt(area);
}

std::vector<SamplePoint> reference_samples(const Grid& reference,
                                           uint64_t budget) {
    const auto info = reference.info();
    std::vector<SamplePoint> result;
    result.reserve(static_cast<size_t>(budget));
    for (uint64_t index = 1; index <= budget; ++index) {
        const double x = info.bounds.xmin +
            halton(index, 2) * (info.bounds.xmax - info.bounds.xmin);
        const double y = info.bounds.ymin +
            halton(index, 3) * (info.bounds.ymax - info.bounds.ymin);
        double z = 0.0;
        if (sample_grid(reference, x, y, z)) result.push_back({x, y, z});
    }
    return result;
}

struct CandidateScore {
    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;
    double rmse = std::numeric_limits<double>::infinity();
    uint64_t count = 0;
};

CandidateScore score_candidate(const std::vector<SamplePoint>& samples,
                               const Grid& moving, double dx, double dy,
                               bool estimate_z) {
    CandidateScore result;
    result.dx = dx;
    result.dy = dy;
    double sum = 0.0;
    std::vector<double> differences;
    differences.reserve(samples.size());
    for (const auto& sample : samples) {
        double moving_z = 0.0;
        if (!sample_grid(moving, sample.x - dx, sample.y - dy, moving_z))
            continue;
        const double difference = sample.z - moving_z;
        differences.push_back(difference);
        sum += difference;
    }
    result.count = differences.size();
    if (differences.empty()) return result;
    result.dz = estimate_z ? sum / static_cast<double>(result.count) : 0.0;
    double squared = 0.0;
    for (double difference : differences) {
        const double residual = difference - result.dz;
        squared += residual * residual;
    }
    result.rmse = std::sqrt(squared / static_cast<double>(result.count));
    return result;
}

bool better_candidate(const CandidateScore& candidate,
                      const CandidateScore& current,
                      uint64_t minimum_count) {
    if (candidate.count < minimum_count) return false;
    if (!finite(current.rmse)) return true;
    const double tolerance = 1e-14 *
        std::max({1.0, candidate.rmse, current.rmse});
    if (candidate.rmse < current.rmse - tolerance) return true;
    if (std::abs(candidate.rmse - current.rmse) <= tolerance) {
        return std::hypot(candidate.dx, candidate.dy) <
               std::hypot(current.dx, current.dy);
    }
    return false;
}

} // namespace

std::unique_ptr<SurfaceClipData> clip_tin_polygon_exact(
    Context& tin, const dt_polygon_rings& polygon) {
    const auto clip = copy_polygon(polygon);
    bg::model::box<Point2> box;
    bg::envelope(clip, box);
    const dt_bounds2 bounds{bg::get<bg::min_corner, 0>(box),
                            bg::get<bg::min_corner, 1>(box),
                            bg::get<bg::max_corner, 0>(box),
                            bg::get<bg::max_corner, 1>(box)};
    auto candidates = tin.query(bounds);
    return clip_surface(clip, candidates->generation, [&](const auto& visitor) {
        for (const auto& triangle : candidates->triangles) visitor(triangle);
    });
}

std::unique_ptr<SurfaceClipData> clip_cdt_polygon_exact(
    CdtContext& cdt, const dt_polygon_rings& polygon) {
    const auto clip = copy_polygon(polygon);
    bg::model::box<Point2> box;
    bg::envelope(clip, box);
    const dt_bounds2 bounds{bg::get<bg::min_corner, 0>(box),
                            bg::get<bg::min_corner, 1>(box),
                            bg::get<bg::max_corner, 0>(box),
                            bg::get<bg::max_corner, 1>(box)};
    auto candidates = cdt.query(bounds);
    return clip_surface(clip, candidates->generation, [&](const auto& visitor) {
        for (const auto& triangle : candidates->triangles) visitor(triangle);
    });
}

dt_surface_registration_result register_grid_surfaces(
    const Grid& reference, const Grid& moving,
    const dt_surface_registration_options& options) {
    require_compatible_crs(reference, moving);
    const uint32_t known_flags = DT_SURFACE_REGISTRATION_ESTIMATE_Z_OFFSET;
    if ((options.flags & ~known_flags) != 0 ||
        !finite(options.maximum_xy_shift) || options.maximum_xy_shift < 0.0 ||
        !finite(options.minimum_xy_step) || options.minimum_xy_step < 0.0)
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid surface registration options");
    const uint64_t budget = options.sample_budget == 0 ? 4096 :
                            options.sample_budget;
    const uint64_t minimum = options.minimum_valid_samples == 0 ? 64 :
                             options.minimum_valid_samples;
    if (budget > 10000000 || minimum > budget)
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid surface registration sample budget");
    const double pixel = grid_pixel_scale(reference);
    const double maximum = options.maximum_xy_shift == 0.0
        ? 5.0 * pixel : options.maximum_xy_shift;
    const double minimum_step = options.minimum_xy_step == 0.0
        ? pixel / 32.0 : options.minimum_xy_step;
    if (!(maximum > 0.0) || !(minimum_step > 0.0) || minimum_step > maximum ||
        minimum_step < std::ldexp(maximum, -40))
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid surface registration search scales");
    const auto samples = reference_samples(reference, budget);
    if (samples.size() < minimum)
        throw Exception(DT_E_NOT_FOUND,
                        "reference surface has too few valid samples");
    const bool estimate_z =
        (options.flags & DT_SURFACE_REGISTRATION_ESTIMATE_Z_OFFSET) != 0;
    const auto before = score_candidate(samples, moving, 0.0, 0.0, false);
    CandidateScore best;
    uint64_t candidates = 0;
    uint64_t iterations = 1;
    const double initial_step = maximum / 2.0;
    for (int yi = -2; yi <= 2; ++yi) {
        for (int xi = -2; xi <= 2; ++xi) {
            const auto candidate = score_candidate(
                samples, moving, xi * initial_step, yi * initial_step,
                estimate_z);
            ++candidates;
            if (better_candidate(candidate, best, minimum)) best = candidate;
        }
    }
    if (!finite(best.rmse))
        throw Exception(DT_E_NOT_FOUND,
                        "surfaces have too few overlapping samples");
    double step = initial_step / 2.0;
    while (step >= minimum_step) {
        const auto center = best;
        for (int yi = -1; yi <= 1; ++yi) {
            for (int xi = -1; xi <= 1; ++xi) {
                if (xi == 0 && yi == 0) continue;
                const double dx = std::clamp(center.dx + xi * step,
                                             -maximum, maximum);
                const double dy = std::clamp(center.dy + yi * step,
                                             -maximum, maximum);
                const auto candidate = score_candidate(samples, moving, dx, dy,
                                                       estimate_z);
                ++candidates;
                if (better_candidate(candidate, best, minimum)) best = candidate;
            }
        }
        ++iterations;
        step *= 0.5;
    }
    dt_surface_registration_result result{};
    result.struct_size = sizeof(result);
    result.flags = options.flags;
    result.dx = best.dx;
    result.dy = best.dy;
    result.dz = best.dz;
    result.rmse_before = before.count >= minimum ? before.rmse :
                         std::numeric_limits<double>::quiet_NaN();
    result.rmse_after = best.rmse;
    result.overlap_ratio = static_cast<double>(best.count) /
                           static_cast<double>(samples.size());
    result.valid_sample_count = best.count;
    result.candidate_count = candidates;
    result.iteration_count = iterations;
    return result;
}

dt_surface_error_result compare_grid_surfaces_adaptive(
    const Grid& reference, const Grid& moving,
    const dt_surface_error_options& options) {
    require_compatible_crs(reference, moving);
    if ((options.flags & ~DT_SURFACE_ERROR_APPLY_REGISTRATION) != 0 ||
        !finite(options.target_rmse_standard_error) ||
        options.target_rmse_standard_error < 0.0 || !finite(options.dx) ||
        !finite(options.dy) || !finite(options.dz))
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid surface error options");
    const uint64_t minimum = options.minimum_samples == 0 ? 1024 :
                             options.minimum_samples;
    const uint64_t maximum = options.maximum_samples == 0 ? 65536 :
                             options.maximum_samples;
    if (minimum == 0 || maximum < minimum || maximum > 100000000)
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid adaptive surface sample limits");
    const auto info = reference.info();
    const bool apply = (options.flags &
                        DT_SURFACE_ERROR_APPLY_REGISTRATION) != 0;
    double sum = 0.0;
    double sum_abs = 0.0;
    double sum2 = 0.0;
    double sum4 = 0.0;
    double minimum_error = std::numeric_limits<double>::infinity();
    double maximum_error = -std::numeric_limits<double>::infinity();
    double maximum_absolute = 0.0;
    uint64_t valid = 0;
    uint64_t attempted = 0;
    double standard_error = std::numeric_limits<double>::infinity();
    bool converged = false;
    for (uint64_t index = 1; index <= maximum; ++index) {
        ++attempted;
        const double x = info.bounds.xmin +
            halton(index, 2) * (info.bounds.xmax - info.bounds.xmin);
        const double y = info.bounds.ymin +
            halton(index, 3) * (info.bounds.ymax - info.bounds.ymin);
        double reference_z = 0.0;
        double moving_z = 0.0;
        const double dx = apply ? options.dx : 0.0;
        const double dy = apply ? options.dy : 0.0;
        const double dz = apply ? options.dz : 0.0;
        if (sample_grid(reference, x, y, reference_z) &&
            sample_grid(moving, x - dx, y - dy, moving_z)) {
            const double error = reference_z - (moving_z + dz);
            const double square = error * error;
            ++valid;
            sum += error;
            sum_abs += std::abs(error);
            sum2 += square;
            sum4 += square * square;
            minimum_error = std::min(minimum_error, error);
            maximum_error = std::max(maximum_error, error);
            maximum_absolute = std::max(maximum_absolute, std::abs(error));
            if (valid >= minimum && valid % 256 == 0) {
                const double mean2 = sum2 / static_cast<double>(valid);
                const double variance2 = std::max(
                    0.0, sum4 / static_cast<double>(valid) - mean2 * mean2);
                const double rmse = std::sqrt(mean2);
                standard_error = rmse == 0.0 ? 0.0 :
                    std::sqrt(variance2 / static_cast<double>(valid)) /
                    (2.0 * rmse);
                if (options.target_rmse_standard_error > 0.0 &&
                    standard_error <= options.target_rmse_standard_error) {
                    converged = true;
                    break;
                }
            }
        }
    }
    if (valid < minimum)
        throw Exception(DT_E_NOT_FOUND,
                        "surfaces have too few jointly valid samples");
    const double mean2 = sum2 / static_cast<double>(valid);
    const double rmse = std::sqrt(mean2);
    const double variance2 = std::max(
        0.0, sum4 / static_cast<double>(valid) - mean2 * mean2);
    standard_error = rmse == 0.0 ? 0.0 :
        std::sqrt(variance2 / static_cast<double>(valid)) / (2.0 * rmse);
    dt_surface_error_result result{};
    result.struct_size = sizeof(result);
    result.flags = options.flags |
        (converged ? static_cast<uint32_t>(DT_SURFACE_ERROR_CONVERGED) : 0u);
    result.attempted_sample_count = attempted;
    result.valid_sample_count = valid;
    result.overlap_ratio = static_cast<double>(valid) /
                           static_cast<double>(attempted);
    result.minimum_error = minimum_error;
    result.maximum_error = maximum_error;
    result.mean_error = sum / static_cast<double>(valid);
    result.mean_absolute_error = sum_abs / static_cast<double>(valid);
    result.rmse = rmse;
    result.maximum_absolute_error = maximum_absolute;
    result.rmse_standard_error = standard_error;
    return result;
}

std::unique_ptr<Grid> apply_grid_registration(
    const Grid& moving, const Grid& reference,
    const dt_surface_registration_result& registration) {
    require_compatible_crs(reference, moving);
    if (!finite(registration.dx) || !finite(registration.dy) ||
        !finite(registration.dz))
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid surface registration result");
    const auto reference_info = reference.info();
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = DT_GRID_HAS_NODATA;
    create.width = reference_info.width;
    create.height = reference_info.height;
    std::copy(std::begin(reference_info.geo_transform),
              std::end(reference_info.geo_transform), create.geo_transform);
    create.nodata_value = std::numeric_limits<double>::quiet_NaN();
    auto output = std::make_unique<Grid>(create);
    std::vector<double> row(static_cast<size_t>(create.width),
                            create.nodata_value);
    for (uint64_t y = 0; y < create.height; ++y) {
        for (uint64_t x = 0; x < create.width; ++x) {
            const auto point = output->point(x, y, 0.0);
            double z = 0.0;
            if (sample_grid(moving, point.x - registration.dx,
                            point.y - registration.dy, z))
                row[static_cast<size_t>(x)] = z + registration.dz;
            else
                row[static_cast<size_t>(x)] = create.nodata_value;
        }
        output->write_window(0, y, create.width, 1, row.data(), create.width);
    }
    output->set_crs_wkt(reference.crs_wkt());
    return output;
}

} // namespace dt
