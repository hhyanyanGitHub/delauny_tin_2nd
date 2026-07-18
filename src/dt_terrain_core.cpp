#include "dt_terrain_core.hpp"
#include "dt_cdt_core.hpp"
#include "dt_surface_analysis.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace dt {
namespace {

constexpr uint64_t kMaximumGridValues = 1000000000ULL;
constexpr uint64_t kMaximumContourLevels = 1000000ULL;
constexpr uint64_t kMaximumContourLines = 100000000ULL;
constexpr uint64_t kMaximumContourVertices = 1000000000ULL;

void check_cancelled(const CancelCallback& cancelled) {
    if (cancelled && cancelled()) {
        throw Exception(DT_E_CANCELLED, "terrain operation was cancelled");
    }
}

void report_progress(const ProgressCallback& progress, double value) {
    if (progress) progress(std::clamp(value, 0.0, 1.0));
}

void validate_stitch_scale(double extent, double tolerance) {
    if (std::isfinite(tolerance) == 0 || tolerance <= 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "contour stitch_tolerance must be positive");
    }
    if (extent > tolerance *
                     (static_cast<double>(std::numeric_limits<int64_t>::max()) /
                      4.0)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "contour stitch_tolerance is too small for the extent");
    }
}

bool finite(double value) noexcept {
    return std::isfinite(value) != 0;
}

void require_file_name(const char* file_name) {
    if (!file_name || *file_name == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "file name is empty");
    }
}

double parse_double_token(const std::string& token, const char* field,
                          bool allow_nan = false) {
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (!end || end == token.c_str() || *end != '\0' ||
        !(finite(value) || (allow_nan && std::isnan(value)))) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        std::string("invalid ") + field + " value");
    }
    return value;
}

void validate_transform(const double* transform) {
    for (int i = 0; i < 6; ++i) {
        if (!finite(transform[i])) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "grid geo_transform must be finite");
        }
    }
    const double determinant =
        transform[1] * transform[5] - transform[2] * transform[4];
    if (!finite(determinant) || determinant == 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "grid geo_transform is singular");
    }
}

double interpolate_edge_z(const dt_segment3& edge, double x, double y) {
    const auto& a = edge.vertex[0].point;
    const auto& b = edge.vertex[1].point;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared == 0.0) return a.z;
    const double t = ((x - a.x) * dx + (y - a.y) * dy) / length_squared;
    return a.z + t * (b.z - a.z);
}

double interpolate_triangle_z(const dt_triangle3& triangle, double x,
                              double y) {
    const auto& a = triangle.vertex[0].point;
    const auto& b = triangle.vertex[1].point;
    const auto& c = triangle.vertex[2].point;
    const double denominator =
        (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (denominator == 0.0) {
        throw Exception(DT_E_INTERNAL, "degenerate TIN triangle");
    }
    const double wa =
        ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) /
        denominator;
    const double wb =
        ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) /
        denominator;
    return wa * a.z + wb * b.z + (1.0 - wa - wb) * c.z;
}

struct Segment {
    dt_point3 a{};
    dt_point3 b{};
};

struct PointKey {
    int64_t x = 0;
    int64_t y = 0;

    bool operator==(const PointKey& other) const noexcept {
        return x == other.x && y == other.y;
    }
};

struct PointKeyHash {
    size_t operator()(const PointKey& key) const noexcept {
        const auto a = static_cast<uint64_t>(key.x);
        const auto b = static_cast<uint64_t>(key.y);
        return static_cast<size_t>(a ^ (b + 0x9e3779b97f4a7c15ULL +
                                        (a << 6U) + (a >> 2U)));
    }
};

struct ExactPointKey {
    double x = 0.0;
    double y = 0.0;

    bool operator==(const ExactPointKey& other) const noexcept {
        return x == other.x && y == other.y;
    }
};

struct ExactPointKeyHash {
    size_t operator()(const ExactPointKey& key) const noexcept {
        const size_t a = std::hash<double>{}(key.x);
        const size_t b = std::hash<double>{}(key.y);
        return a ^ (b + static_cast<size_t>(0x9e3779b9U) + (a << 6U) +
                    (a >> 2U));
    }
};

PointKey point_key(const dt_point3& point, double origin_x, double origin_y,
                   double tolerance) {
    return {static_cast<int64_t>(std::llround((point.x - origin_x) / tolerance)),
            static_cast<int64_t>(std::llround((point.y - origin_y) / tolerance))};
}

std::vector<double> make_levels(double minimum, double maximum,
                                const dt_contour_options& options) {
    if (!finite(minimum) || !finite(maximum) || minimum > maximum) {
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid contour Z range");
    }
    std::vector<double> levels;
    if (options.level_count != 0) {
        if (!options.levels) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "contour levels pointer is null");
        }
        if (options.level_count > kMaximumContourLevels) {
            throw Exception(DT_E_LIMIT_EXCEEDED, "too many contour levels");
        }
        levels.assign(options.levels, options.levels + options.level_count);
        for (double level : levels) {
            if (!finite(level)) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "contour levels must be finite");
            }
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        return levels;
    }

    if (!finite(options.interval) || options.interval <= 0.0 ||
        !finite(options.base)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "contour interval must be positive and base finite");
    }
    const double first_index =
        std::ceil((minimum - options.base) / options.interval);
    const double last_index =
        std::floor((maximum - options.base) / options.interval);
    if (!finite(first_index) || !finite(last_index) ||
        last_index < first_index) {
        return levels;
    }
    const long double count =
        static_cast<long double>(last_index) - first_index + 1.0L;
    if (count > static_cast<long double>(kMaximumContourLevels)) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "too many contour levels");
    }
    levels.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < static_cast<uint64_t>(count); ++i) {
        levels.push_back(options.base +
                         (first_index + static_cast<double>(i)) *
                             options.interval);
    }
    return levels;
}

void add_triangle_segments(const dt_triangle3& triangle,
                           const std::vector<double>& levels,
                           std::vector<std::vector<Segment>>& segments,
                           double tolerance) {
    const double zmin = std::min({triangle.vertex[0].point.z,
                                  triangle.vertex[1].point.z,
                                  triangle.vertex[2].point.z});
    const double zmax = std::max({triangle.vertex[0].point.z,
                                  triangle.vertex[1].point.z,
                                  triangle.vertex[2].point.z});
    auto begin = std::lower_bound(levels.begin(), levels.end(), zmin);
    auto end = std::upper_bound(levels.begin(), levels.end(), zmax);
    for (auto level_it = begin; level_it != end; ++level_it) {
        const double level = *level_it;
        std::array<dt_point3, 3> intersections{};
        size_t count = 0;
        for (size_t edge = 0; edge < 3; ++edge) {
            const auto& a = triangle.vertex[edge].point;
            const auto& b = triangle.vertex[(edge + 1) % 3].point;
            const bool a_above = a.z > level;
            const bool b_above = b.z > level;
            if (a_above == b_above) continue;
            const double t = (level - a.z) / (b.z - a.z);
            intersections[count++] = {
                a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), level};
        }
        if (count != 2) continue;
        const double dx = intersections[1].x - intersections[0].x;
        const double dy = intersections[1].y - intersections[0].y;
        if (dx * dx + dy * dy <= tolerance * tolerance) continue;
        const size_t index =
            static_cast<size_t>(level_it - levels.begin());
        segments[index].push_back({intersections[0], intersections[1]});
    }
}

void stitch_level(double level, const std::vector<Segment>& segments,
                  double origin_x, double origin_y, double tolerance,
                  ContourSet& output) {
    struct Endpoint {
        size_t segment = 0;
        uint8_t end = 0;
    };
    std::unordered_map<PointKey, std::vector<Endpoint>, PointKeyHash> adjacency;
    adjacency.reserve(segments.size() * 2);
    for (size_t i = 0; i < segments.size(); ++i) {
        adjacency[point_key(segments[i].a, origin_x, origin_y, tolerance)]
            .push_back({i, 0});
        adjacency[point_key(segments[i].b, origin_x, origin_y, tolerance)]
            .push_back({i, 1});
    }
    std::vector<uint8_t> used(segments.size(), 0);

    auto append_path = [&](size_t first_segment, uint8_t first_end) {
        ContourLine line;
        line.elevation = level;
        size_t segment_index = first_segment;
        uint8_t entry_end = first_end;
        const dt_point3 start = entry_end == 0 ? segments[segment_index].a
                                               : segments[segment_index].b;
        line.points.push_back(start);
        PointKey start_key = point_key(start, origin_x, origin_y, tolerance);
        while (!used[segment_index]) {
            used[segment_index] = 1;
            const dt_point3 next = entry_end == 0 ? segments[segment_index].b
                                                  : segments[segment_index].a;
            line.points.push_back(next);
            const PointKey next_key =
                point_key(next, origin_x, origin_y, tolerance);
            if (next_key == start_key) {
                line.flags |= DT_CONTOUR_LINE_CLOSED;
                break;
            }
            const auto found = adjacency.find(next_key);
            if (found == adjacency.end()) break;
            bool advanced = false;
            for (const Endpoint endpoint : found->second) {
                if (!used[endpoint.segment]) {
                    segment_index = endpoint.segment;
                    entry_end = endpoint.end;
                    advanced = true;
                    break;
                }
            }
            if (!advanced) break;
        }
        if (line.points.size() >= 2) output.lines.push_back(std::move(line));
    };

    for (const auto& entry : adjacency) {
        if (entry.second.size() == 2) continue;
        for (const Endpoint endpoint : entry.second) {
            if (!used[endpoint.segment]) {
                append_path(endpoint.segment, endpoint.end);
            }
        }
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        if (!used[i]) append_path(i, 0);
    }
}

} // namespace

Grid::Grid(const dt_grid_create_options& options) : options_(options) {
    if (options_.width == 0 || options_.height == 0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "grid width and height must be positive");
    }
    if (options_.width > kMaximumGridValues / options_.height) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "grid is too large");
    }
    const uint64_t count = options_.width * options_.height;
    if (count > kMaximumGridValues ||
        count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "grid is too large");
    }
    validate_transform(options_.geo_transform);
    if ((options_.flags & DT_GRID_HAS_NODATA) != 0 &&
        !(finite(options_.nodata_value) || std::isnan(options_.nodata_value))) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "grid nodata value must be finite or NaN");
    }
    const double initial = (options_.flags & DT_GRID_HAS_NODATA) != 0
                               ? options_.nodata_value
                               : 0.0;
    values_.assign(static_cast<size_t>(count), initial);
}

size_t Grid::offset(uint64_t column, uint64_t row) const {
    return static_cast<size_t>(row * options_.width + column);
}

void Grid::validate_window(uint64_t column, uint64_t row, uint64_t width,
                           uint64_t height, uint64_t stride) const {
    if (width == 0 || height == 0 || column > options_.width ||
        row > options_.height || width > options_.width - column ||
        height > options_.height - row || stride < width) {
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid grid window");
    }
}

bool Grid::is_nodata(double value) const noexcept {
    if ((options_.flags & DT_GRID_HAS_NODATA) == 0) return false;
    if (std::isnan(options_.nodata_value)) return std::isnan(value) != 0;
    return value == options_.nodata_value;
}

dt_point3 Grid::point(uint64_t column, uint64_t row, double z) const noexcept {
    const double c = static_cast<double>(column);
    const double r = static_cast<double>(row);
    return {options_.geo_transform[0] + c * options_.geo_transform[1] +
                r * options_.geo_transform[2],
            options_.geo_transform[3] + c * options_.geo_transform[4] +
                r * options_.geo_transform[5],
            z};
}

dt_surface_analysis Grid::analyze_surface_xy(
    const dt_point3& query) const {
    if (!std::isfinite(query.x) || !std::isfinite(query.y)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "GRID analysis query must be finite");
    }
    if (options_.width < 2 || options_.height < 2) {
        throw Exception(DT_E_EMPTY,
                        "GRID needs at least two rows and columns");
    }
    const double* gt = options_.geo_transform;
    const double determinant = gt[1] * gt[5] - gt[2] * gt[4];
    const double dx = query.x - gt[0];
    const double dy = query.y - gt[3];
    double column = (dx * gt[5] - dy * gt[2]) / determinant;
    double row = (dy * gt[1] - dx * gt[4]) / determinant;
    const double max_column = static_cast<double>(options_.width - 1);
    const double max_row = static_cast<double>(options_.height - 1);
    const double scale = std::max({1.0, std::abs(column), std::abs(row),
                                   max_column, max_row});
    const double tolerance =
        std::numeric_limits<double>::epsilon() * scale * 64.0;
    if (column < -tolerance || column > max_column + tolerance ||
        row < -tolerance || row > max_row + tolerance) {
        throw Exception(DT_E_NOT_FOUND, "query point is outside the GRID");
    }
    column = std::clamp(column, 0.0, max_column);
    row = std::clamp(row, 0.0, max_row);
    uint64_t cell_column = static_cast<uint64_t>(std::floor(column));
    uint64_t cell_row = static_cast<uint64_t>(std::floor(row));
    if (cell_column + 1 >= options_.width) cell_column = options_.width - 2;
    if (cell_row + 1 >= options_.height) cell_row = options_.height - 2;
    const double u = column - static_cast<double>(cell_column);
    const double v = row - static_cast<double>(cell_row);
    const double z00 = values_[offset(cell_column, cell_row)];
    const double z10 = values_[offset(cell_column + 1, cell_row)];
    const double z01 = values_[offset(cell_column, cell_row + 1)];
    const double z11 = values_[offset(cell_column + 1, cell_row + 1)];
    const double values[4] = {z00, z10, z01, z11};
    for (const double value : values) {
        if (!std::isfinite(value) || is_nodata(value)) {
            throw Exception(DT_E_NOT_FOUND,
                            "GRID analysis support cell contains NoData");
        }
    }
    const double z = (1.0 - u) * (1.0 - v) * z00 +
                     u * (1.0 - v) * z10 +
                     (1.0 - u) * v * z01 + u * v * z11;
    const double dz_dcolumn =
        (1.0 - v) * (z10 - z00) + v * (z11 - z01);
    const double dz_drow =
        (1.0 - u) * (z01 - z00) + u * (z11 - z10);
    const double dz_dx =
        (dz_dcolumn * gt[5] - gt[4] * dz_drow) / determinant;
    const double dz_dy =
        (gt[1] * dz_drow - gt[2] * dz_dcolumn) / determinant;
    const dt_point3 support[4] = {
        point(cell_column, cell_row, z00),
        point(cell_column + 1, cell_row, z10),
        point(cell_column, cell_row + 1, z01),
        point(cell_column + 1, cell_row + 1, z11)};
    return make_surface_analysis(query, z, dz_dx, dz_dy, support, 4,
                                 DT_SURFACE_BILINEAR);
}

dt_grid_info Grid::info() const {
    dt_grid_info result{};
    result.struct_size = sizeof(result);
    result.flags = options_.flags;
    result.width = options_.width;
    result.height = options_.height;
    std::copy(std::begin(options_.geo_transform),
              std::end(options_.geo_transform), result.geo_transform);
    result.nodata_value = options_.nodata_value;
    const std::array<dt_point3, 4> corners{
        point(0, 0, 0.0), point(options_.width - 1, 0, 0.0),
        point(0, options_.height - 1, 0.0),
        point(options_.width - 1, options_.height - 1, 0.0)};
    result.bounds = {corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const auto& corner : corners) {
        result.bounds.xmin = std::min(result.bounds.xmin, corner.x);
        result.bounds.ymin = std::min(result.bounds.ymin, corner.y);
        result.bounds.xmax = std::max(result.bounds.xmax, corner.x);
        result.bounds.ymax = std::max(result.bounds.ymax, corner.y);
    }
    result.valid_value_count = static_cast<uint64_t>(
        std::count_if(values_.begin(), values_.end(),
                      [&](double value) { return !is_nodata(value); }));
    result.generation = generation_;
    return result;
}

std::unique_ptr<Grid> grid_derive_terrain(
    const Grid& source, const dt_grid_terrain_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    if (source.width() < 2 || source.height() < 2) {
        throw Exception(DT_E_EMPTY,
                        "terrain analysis needs at least two rows and columns");
    }
    if (options.kind != DT_GRID_TERRAIN_SLOPE_DEGREES &&
        options.kind != DT_GRID_TERRAIN_ASPECT_DEGREES &&
        options.kind != DT_GRID_TERRAIN_HILLSHADE) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "unknown GRID terrain analysis kind");
    }
    const double z_factor = options.z_factor == 0.0 ? 1.0 : options.z_factor;
    if (!finite(z_factor) || z_factor <= 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "terrain analysis z_factor must be positive");
    }
    const double output_nodata = options.output_nodata_value == 0.0
                                     ? std::numeric_limits<double>::quiet_NaN()
                                     : options.output_nodata_value;
    if (!(finite(output_nodata) || std::isnan(output_nodata))) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "terrain analysis NoData must be finite or NaN");
    }

    double sun_azimuth = options.sun_azimuth_degrees;
    double sun_altitude = options.sun_altitude_degrees;
    if (sun_azimuth == 0.0 && sun_altitude == 0.0) {
        sun_azimuth = 315.0;
        sun_altitude = 45.0;
    }
    if (!finite(sun_azimuth) || !finite(sun_altitude) ||
        sun_altitude < -90.0 || sun_altitude > 90.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid hillshade illumination angles");
    }
    if (options.worker_count > 64) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "terrain analysis worker_count exceeds 64");
    }
    constexpr uint32_t kDefaultTileRows = 64;
    constexpr uint32_t kMaximumTileRows = 1024U * 1024U;
    const uint32_t tile_rows = options.tile_row_count == 0
                                   ? kDefaultTileRows
                                   : options.tile_row_count;
    if (tile_rows > kMaximumTileRows) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "terrain analysis tile_row_count is too large");
    }

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = DT_GRID_HAS_NODATA;
    create.width = source.width();
    create.height = source.height();
    std::copy(source.transform(), source.transform() + 6,
              create.geo_transform);
    create.nodata_value = output_nodata;
    auto output = std::make_unique<Grid>(create);
    output->set_crs_wkt(source.crs_wkt());

    const double* gt = source.transform();
    const double determinant = gt[1] * gt[5] - gt[2] * gt[4];
    constexpr double kPi = 3.141592653589793238462643383279502884;
    const double azimuth_radians = sun_azimuth * kPi / 180.0;
    const double altitude_radians = sun_altitude * kPi / 180.0;
    const double sun_x = std::sin(azimuth_radians) *
                         std::cos(altitude_radians);
    const double sun_y = std::cos(azimuth_radians) *
                         std::cos(altitude_radians);
    const double sun_z = std::sin(altitude_radians);
    const auto& values = source.values();
    const uint64_t width = source.width();
    const uint64_t height = source.height();
    const auto value_at = [&](uint64_t column, uint64_t row) {
        return values[static_cast<size_t>(row * width + column)];
    };
    const auto valid = [&](double value) {
        return finite(value) && !source.is_nodata(value);
    };
    const auto compute_row = [&](uint64_t row,
                                 const std::atomic<bool>* stop_requested) {
        const uint64_t previous_row = row == 0 ? row : row - 1;
        const uint64_t next_row = row + 1 < height ? row + 1 : row;
        const double row_span = static_cast<double>(next_row - previous_row);
        double* row_output = output->values_.data() +
                             static_cast<size_t>(row * width);
        for (uint64_t column = 0; column < width; ++column) {
            if (stop_requested && (column & 4095U) == 0U &&
                stop_requested->load()) {
                return false;
            }
            const uint64_t previous_column = column == 0 ? column : column - 1;
            const uint64_t next_column =
                column + 1 < width ? column + 1 : column;
            const double column_span =
                static_cast<double>(next_column - previous_column);
            const double center = value_at(column, row);
            const double left = value_at(previous_column, row);
            const double right = value_at(next_column, row);
            const double above = value_at(column, previous_row);
            const double below = value_at(column, next_row);
            if (!valid(center) || !valid(left) || !valid(right) ||
                !valid(above) || !valid(below)) {
                row_output[static_cast<size_t>(column)] = create.nodata_value;
                continue;
            }
            const double dz_dcolumn =
                z_factor * (right - left) / column_span;
            const double dz_drow = z_factor * (below - above) / row_span;
            const double dz_dx =
                (dz_dcolumn * gt[5] - gt[4] * dz_drow) / determinant;
            const double dz_dy =
                (gt[1] * dz_drow - gt[2] * dz_dcolumn) / determinant;
            const double gradient = std::hypot(dz_dx, dz_dy);
            double derived = create.nodata_value;
            if (options.kind == DT_GRID_TERRAIN_SLOPE_DEGREES) {
                derived = std::atan(gradient) * 180.0 / kPi;
            } else if (options.kind == DT_GRID_TERRAIN_ASPECT_DEGREES) {
                if (gradient > 0.0) {
                    derived = std::atan2(-dz_dx, -dz_dy) * 180.0 / kPi;
                    if (derived < 0.0) derived += 360.0;
                }
            } else {
                const double length = std::hypot(gradient, 1.0);
                const double illumination =
                    (-dz_dx * sun_x - dz_dy * sun_y + sun_z) / length;
                derived = 255.0 * std::max(0.0, illumination);
            }
            row_output[static_cast<size_t>(column)] = derived;
        }
        return true;
    };

    const uint64_t tile_count =
        (height + static_cast<uint64_t>(tile_rows) - 1U) / tile_rows;
    uint32_t worker_count = options.worker_count;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) worker_count = 4;
        worker_count = std::min(worker_count, 32U);
    }
    worker_count = static_cast<uint32_t>(std::min<uint64_t>(
        worker_count, std::max<uint64_t>(1, tile_count)));

    report_progress(progress, 0.0);
    if (worker_count == 1) {
        for (uint64_t row = 0; row < height; ++row) {
            check_cancelled(cancelled);
            compute_row(row, nullptr);
            report_progress(progress,
                            static_cast<double>(row + 1) /
                                static_cast<double>(height));
        }
    } else {
        std::atomic<uint64_t> next_row{0};
        std::atomic<uint64_t> completed_rows{0};
        std::atomic<uint32_t> active_workers{worker_count};
        std::atomic<bool> stop_requested{false};
        std::mutex event_mutex;
        std::condition_variable event;
        std::mutex error_mutex;
        std::exception_ptr worker_error;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        const auto worker = [&] {
            try {
                while (!stop_requested.load()) {
                    const uint64_t begin = next_row.fetch_add(tile_rows);
                    if (begin >= height) break;
                    const uint64_t end = std::min<uint64_t>(
                        height, begin + static_cast<uint64_t>(tile_rows));
                    for (uint64_t row = begin; row < end; ++row) {
                        if (stop_requested.load() ||
                            !compute_row(row, &stop_requested)) {
                            break;
                        }
                        completed_rows.fetch_add(1);
                    }
                    event.notify_one();
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!worker_error) worker_error = std::current_exception();
                }
                stop_requested.store(true);
            }
            active_workers.fetch_sub(1);
            event.notify_one();
        };

        try {
            for (uint32_t i = 0; i < worker_count; ++i)
                workers.emplace_back(worker);
        } catch (...) {
            stop_requested.store(true);
            for (auto& thread : workers)
                if (thread.joinable()) thread.join();
            throw;
        }

        bool cancellation_seen = false;
        std::exception_ptr coordinator_error;
        try {
            while (active_workers.load() != 0) {
                if (cancelled && cancelled()) {
                    cancellation_seen = true;
                    stop_requested.store(true);
                }
                report_progress(
                    progress,
                    static_cast<double>(completed_rows.load()) /
                        static_cast<double>(height));
                std::unique_lock<std::mutex> lock(event_mutex);
                event.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return active_workers.load() == 0;
                });
            }
        } catch (...) {
            coordinator_error = std::current_exception();
            stop_requested.store(true);
        }
        for (auto& thread : workers)
            if (thread.joinable()) thread.join();
        if (coordinator_error) std::rethrow_exception(coordinator_error);
        if (worker_error) std::rethrow_exception(worker_error);
        if (cancellation_seen || (cancelled && cancelled())) {
            throw Exception(DT_E_CANCELLED,
                            "terrain operation was cancelled");
        }
    }
    ++output->generation_;
    report_progress(progress, 1.0);
    return output;
}

GridEarthworkComputation grid_compare_earthwork(
    const Grid& existing, const Grid& design,
    const dt_grid_earthwork_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    if (existing.width() < 2 || existing.height() < 2) {
        throw Exception(DT_E_EMPTY,
                        "earthwork analysis needs at least two rows and columns");
    }
    if (existing.width() != design.width() ||
        existing.height() != design.height()) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork GRID dimensions do not match");
    }
    const auto nearly_equal = [](double a, double b) {
        return std::abs(a - b) <=
               1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
    };
    for (int i = 0; i < 6; ++i) {
        if (!nearly_equal(existing.transform()[i], design.transform()[i])) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "earthwork GRID affine transforms do not match");
        }
    }
    if (existing.crs_wkt() != design.crs_wkt()) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork GRID coordinate systems do not match");
    }
    constexpr uint32_t kKnownFlags =
        DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID |
        DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS;
    if ((options.flags & ~kKnownFlags) != 0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "unknown earthwork analysis flags");
    }
    if (options.worker_count > 64) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork worker_count exceeds 64");
    }
    constexpr uint32_t kDefaultTileRows = 64;
    constexpr uint32_t kMaximumTileRows = 1024U * 1024U;
    const uint32_t tile_rows = options.tile_row_count == 0
                                   ? kDefaultTileRows
                                   : options.tile_row_count;
    if (tile_rows > kMaximumTileRows) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork tile_row_count is too large");
    }
    const double existing_factor = options.existing_z_factor == 0.0
                                       ? 1.0
                                       : options.existing_z_factor;
    const double design_factor = options.design_z_factor == 0.0
                                     ? 1.0
                                     : options.design_z_factor;
    if (!finite(existing_factor) || existing_factor <= 0.0 ||
        !finite(design_factor) || design_factor <= 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork z factors must be positive");
    }
    const double output_nodata = options.output_nodata_value == 0.0
                                     ? std::numeric_limits<double>::quiet_NaN()
                                     : options.output_nodata_value;
    if (!(finite(output_nodata) || std::isnan(output_nodata))) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "earthwork output NoData must be finite or NaN");
    }

    GridEarthworkComputation output{};
    output.result.struct_size = sizeof(output.result);
    output.result.flags = options.flags;
    const uint64_t width = existing.width();
    const uint64_t height = existing.height();
    const uint64_t cell_rows = height - 1;
    const uint64_t cell_columns = width - 1;
    output.result.cell_count = cell_rows * cell_columns;
    const double* gt = existing.transform();
    const double cell_area =
        std::abs(gt[1] * gt[5] - gt[2] * gt[4]);
    const double triangle_area = cell_area * 0.5;
    output.result.total_plan_area =
        cell_area * static_cast<double>(output.result.cell_count);

    if ((options.flags & DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID) != 0) {
        dt_grid_create_options create{};
        create.struct_size = sizeof(create);
        create.flags = DT_GRID_HAS_NODATA;
        create.width = width;
        create.height = height;
        std::copy(existing.transform(), existing.transform() + 6,
                  create.geo_transform);
        create.nodata_value = output_nodata;
        output.difference_grid = std::make_unique<Grid>(create);
        output.difference_grid->set_crs_wkt(existing.crs_wkt());
    }

    struct TileStatistics {
        uint64_t valid = 0;
        uint64_t skipped = 0;
        double cut = 0.0;
        double fill = 0.0;
        double net = 0.0;
        double squared = 0.0;
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
    };
    const uint64_t tile_count =
        (cell_rows + static_cast<uint64_t>(tile_rows) - 1U) / tile_rows;
    std::vector<TileStatistics> tile_statistics(
        static_cast<size_t>(tile_count));
    const auto& existing_values = existing.values();
    const auto& design_values = design.values();
    const auto node_difference = [&](uint64_t index, double& difference) {
        const double a = existing_values[static_cast<size_t>(index)];
        const double b = design_values[static_cast<size_t>(index)];
        if (!finite(a) || !finite(b) || existing.is_nodata(a) ||
            design.is_nodata(b)) {
            return false;
        }
        difference = existing_factor * a - design_factor * b;
        return finite(difference);
    };
    const auto accumulate_triangle = [&](const std::array<double, 3>& d,
                                         TileStatistics& statistics) {
        ++statistics.valid;
        statistics.minimum =
            std::min({statistics.minimum, d[0], d[1], d[2]});
        statistics.maximum =
            std::max({statistics.maximum, d[0], d[1], d[2]});
        const double net = triangle_area * (d[0] + d[1] + d[2]) / 3.0;
        const double squared = triangle_area / 6.0 *
            (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] +
             d[0] * d[1] + d[1] * d[2] + d[2] * d[0]);
        statistics.net += net;
        statistics.squared += std::max(0.0, squared);
        int positive_count = 0;
        int negative_count = 0;
        for (double value : d) {
            if (value > 0.0) ++positive_count;
            else if (value < 0.0) ++negative_count;
        }
        double cut = 0.0;
        double fill = 0.0;
        if (negative_count == 0) {
            cut = std::max(0.0, net);
        } else if (positive_count == 0) {
            fill = std::max(0.0, -net);
        } else if (positive_count == 1) {
            double positive = 0.0;
            std::array<double, 2> other{};
            size_t next = 0;
            for (double value : d) {
                if (value > 0.0) positive = value;
                else other[next++] = value;
            }
            const double first = positive / (positive - other[0]);
            const double second = positive / (positive - other[1]);
            cut = triangle_area * first * second * positive / 3.0;
            fill = std::max(0.0, cut - net);
        } else {
            double negative = 0.0;
            std::array<double, 2> other{};
            size_t next = 0;
            for (double value : d) {
                if (value < 0.0) negative = value;
                else other[next++] = value;
            }
            const double first = -negative / (other[0] - negative);
            const double second = -negative / (other[1] - negative);
            fill = triangle_area * first * second * (-negative) / 3.0;
            cut = std::max(0.0, net + fill);
        }
        statistics.cut += cut;
        statistics.fill += fill;
    };

    std::atomic<uint64_t> completed_rows{0};
    const bool partial_cells =
        (options.flags & DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS) != 0;
    const auto process_tile = [&](uint64_t tile_index,
                                  const std::atomic<bool>* stop_requested) {
        TileStatistics local{};
        const uint64_t begin = tile_index * tile_rows;
        const uint64_t end = std::min<uint64_t>(
            cell_rows, begin + static_cast<uint64_t>(tile_rows));
        for (uint64_t row = begin; row < end; ++row) {
            if (stop_requested) {
                if (stop_requested->load()) return false;
            } else {
                check_cancelled(cancelled);
            }
            double* difference_row = output.difference_grid
                ? output.difference_grid->values_.data() +
                      static_cast<size_t>(row * width)
                : nullptr;
            for (uint64_t column = 0; column < width; ++column) {
                if (stop_requested && (column & 4095U) == 0U &&
                    stop_requested->load()) {
                    return false;
                }
                if (difference_row) {
                    double difference = 0.0;
                    difference_row[static_cast<size_t>(column)] =
                        node_difference(row * width + column, difference)
                            ? difference
                            : output_nodata;
                }
                if (column == cell_columns) continue;
                const uint64_t top_left = row * width + column;
                std::array<double, 4> d{};
                std::array<bool, 4> valid_node{
                    node_difference(top_left, d[0]),
                    node_difference(top_left + 1, d[1]),
                    node_difference(top_left + width, d[2]),
                    node_difference(top_left + width + 1, d[3])};
                if (!partial_cells &&
                    (!valid_node[0] || !valid_node[1] ||
                     !valid_node[2] || !valid_node[3])) {
                    local.skipped += 2;
                    continue;
                }
                if (valid_node[0] && valid_node[1] && valid_node[3])
                    accumulate_triangle({d[0], d[1], d[3]}, local);
                else
                    ++local.skipped;
                if (valid_node[0] && valid_node[3] && valid_node[2])
                    accumulate_triangle({d[0], d[3], d[2]}, local);
                else
                    ++local.skipped;
            }
            completed_rows.fetch_add(1);
        }
        tile_statistics[static_cast<size_t>(tile_index)] = local;
        return true;
    };

    uint32_t worker_count = options.worker_count;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) worker_count = 4;
        worker_count = std::min(worker_count, 32U);
    }
    worker_count = static_cast<uint32_t>(std::min<uint64_t>(
        worker_count, std::max<uint64_t>(1, tile_count)));
    report_progress(progress, 0.0);
    if (worker_count == 1) {
        for (uint64_t tile = 0; tile < tile_count; ++tile) {
            process_tile(tile, nullptr);
            report_progress(progress,
                            static_cast<double>(completed_rows.load()) /
                                static_cast<double>(cell_rows));
        }
    } else {
        std::atomic<uint64_t> next_tile{0};
        std::atomic<uint32_t> active_workers{worker_count};
        std::atomic<bool> stop_requested{false};
        std::mutex event_mutex;
        std::condition_variable event;
        std::mutex error_mutex;
        std::exception_ptr worker_error;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        const auto worker = [&] {
            try {
                while (!stop_requested.load()) {
                    const uint64_t tile = next_tile.fetch_add(1);
                    if (tile >= tile_count) break;
                    if (!process_tile(tile, &stop_requested)) break;
                    event.notify_one();
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!worker_error) worker_error = std::current_exception();
                }
                stop_requested.store(true);
            }
            active_workers.fetch_sub(1);
            event.notify_one();
        };
        try {
            for (uint32_t i = 0; i < worker_count; ++i)
                workers.emplace_back(worker);
        } catch (...) {
            stop_requested.store(true);
            for (auto& thread : workers)
                if (thread.joinable()) thread.join();
            throw;
        }
        bool cancellation_seen = false;
        std::exception_ptr coordinator_error;
        try {
            while (active_workers.load() != 0) {
                if (cancelled && cancelled()) {
                    cancellation_seen = true;
                    stop_requested.store(true);
                }
                report_progress(
                    progress,
                    static_cast<double>(completed_rows.load()) /
                        static_cast<double>(cell_rows));
                std::unique_lock<std::mutex> lock(event_mutex);
                event.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return active_workers.load() == 0;
                });
            }
        } catch (...) {
            coordinator_error = std::current_exception();
            stop_requested.store(true);
        }
        for (auto& thread : workers)
            if (thread.joinable()) thread.join();
        if (coordinator_error) std::rethrow_exception(coordinator_error);
        if (worker_error) std::rethrow_exception(worker_error);
        if (cancellation_seen || (cancelled && cancelled())) {
            throw Exception(DT_E_CANCELLED,
                            "earthwork operation was cancelled");
        }
    }

    if (output.difference_grid) {
        check_cancelled(cancelled);
        double* last_row = output.difference_grid->values_.data() +
                           static_cast<size_t>((height - 1) * width);
        for (uint64_t column = 0; column < width; ++column) {
            if ((column & 4095U) == 0U) check_cancelled(cancelled);
            double difference = 0.0;
            last_row[static_cast<size_t>(column)] =
                node_difference((height - 1) * width + column, difference)
                    ? difference
                    : output_nodata;
        }
        ++output.difference_grid->generation_;
    }
    double minimum_difference = std::numeric_limits<double>::infinity();
    double maximum_difference = -std::numeric_limits<double>::infinity();
    double squared_difference_integral = 0.0;
    for (const auto& statistics : tile_statistics) {
        output.result.valid_triangle_count += statistics.valid;
        output.result.skipped_triangle_count += statistics.skipped;
        output.result.cut_volume += statistics.cut;
        output.result.fill_volume += statistics.fill;
        output.result.net_volume += statistics.net;
        squared_difference_integral += statistics.squared;
        minimum_difference =
            std::min(minimum_difference, statistics.minimum);
        maximum_difference =
            std::max(maximum_difference, statistics.maximum);
    }
    output.result.valid_plan_area =
        triangle_area * static_cast<double>(output.result.valid_triangle_count);
    output.result.coverage_ratio = output.result.total_plan_area > 0.0
        ? output.result.valid_plan_area / output.result.total_plan_area
        : 0.0;
    if (output.result.valid_plan_area > 0.0) {
        output.result.minimum_difference = minimum_difference;
        output.result.maximum_difference = maximum_difference;
        output.result.mean_difference =
            output.result.net_volume / output.result.valid_plan_area;
        output.result.rmse_difference = std::sqrt(
            squared_difference_integral / output.result.valid_plan_area);
    } else {
        output.result.minimum_difference =
            std::numeric_limits<double>::quiet_NaN();
        output.result.maximum_difference =
            std::numeric_limits<double>::quiet_NaN();
        output.result.mean_difference =
            std::numeric_limits<double>::quiet_NaN();
        output.result.rmse_difference =
            std::numeric_limits<double>::quiet_NaN();
    }
    report_progress(progress, 1.0);
    return output;
}

void Grid::read_window(uint64_t column, uint64_t row, uint64_t width,
                       uint64_t height, double* output, uint64_t stride) const {
    if (!output) throw Exception(DT_E_INVALID_ARGUMENT, "output_values is null");
    if (stride == 0) stride = width;
    validate_window(column, row, width, height, stride);
    for (uint64_t y = 0; y < height; ++y) {
        const auto begin = values_.begin() +
                           static_cast<std::ptrdiff_t>(offset(column, row + y));
        std::copy_n(begin, static_cast<size_t>(width), output + y * stride);
    }
}

void Grid::write_window(uint64_t column, uint64_t row, uint64_t width,
                        uint64_t height, const double* input, uint64_t stride) {
    if (!input) throw Exception(DT_E_INVALID_ARGUMENT, "values is null");
    if (stride == 0) stride = width;
    validate_window(column, row, width, height, stride);
    for (uint64_t y = 0; y < height; ++y) {
        for (uint64_t x = 0; x < width; ++x) {
            const double value = input[y * stride + x];
            if (!(finite(value) ||
                  ((options_.flags & DT_GRID_HAS_NODATA) != 0 &&
                   std::isnan(options_.nodata_value) && std::isnan(value)))) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "grid values must be finite or NoData NaN");
            }
        }
        std::copy_n(input + y * stride, static_cast<size_t>(width),
                    values_.begin() + static_cast<std::ptrdiff_t>(
                                          offset(column, row + y)));
    }
    ++generation_;
}

void Grid::save_text(const char* file_name) const {
    require_file_name(file_name);
    std::ofstream stream(std::filesystem::u8path(file_name),
                         std::ios::binary | std::ios::trunc);
    if (!stream) throw Exception(DT_E_IO, "cannot open grid text file for writing");
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << "DGRID 1\n"
           << "SIZE " << options_.width << ' ' << options_.height << "\n"
           << "FLAGS " << options_.flags << "\n"
           << "GEOTRANSFORM";
    for (double value : options_.geo_transform) stream << ' ' << value;
    stream << "\nNODATA " << options_.nodata_value << "\nVALUES\n";
    for (uint64_t row = 0; row < options_.height; ++row) {
        for (uint64_t column = 0; column < options_.width; ++column) {
            if (column) stream << ' ';
            stream << values_[offset(column, row)];
        }
        stream << '\n';
    }
    stream << "END\n";
    if (!stream) throw Exception(DT_E_IO, "failed while writing grid text file");
}

std::unique_ptr<Grid> Grid::load_text(const char* file_name) {
    require_file_name(file_name);
    std::ifstream stream(std::filesystem::u8path(file_name), std::ios::binary);
    if (!stream) throw Exception(DT_E_IO, "cannot open grid text file");
    stream.imbue(std::locale::classic());
    std::string token;
    uint32_t version = 0;
    if (!(stream >> token >> version) || token != "DGRID" || version != 1) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DGRID header");
    }
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    if (!(stream >> token >> options.width >> options.height) || token != "SIZE" ||
        !(stream >> token >> options.flags) || token != "FLAGS" ||
        !(stream >> token) || token != "GEOTRANSFORM") {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DGRID metadata");
    }
    for (double& value : options.geo_transform) {
        std::string value_token;
        if (!(stream >> value_token)) {
            throw Exception(DT_E_CORRUPTED_DATA, "truncated DGRID transform");
        }
        value = parse_double_token(value_token, "grid transform");
    }
    std::string nodata_token;
    if (!(stream >> token >> nodata_token) || token != "NODATA") {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DGRID NoData metadata");
    }
    options.nodata_value = parse_double_token(nodata_token, "NoData", true);
    if (!(stream >> token) || token != "VALUES") {
        throw Exception(DT_E_CORRUPTED_DATA, "missing DGRID VALUES section");
    }
    auto grid = std::make_unique<Grid>(options);
    for (double& value : grid->values_) {
        std::string value_token;
        if (!(stream >> value_token)) {
            throw Exception(DT_E_CORRUPTED_DATA, "truncated DGRID values");
        }
        value = parse_double_token(value_token, "grid", true);
        if (!finite(value) &&
            !((options.flags & DT_GRID_HAS_NODATA) != 0 &&
              std::isnan(options.nodata_value) && std::isnan(value))) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGRID contains NaN without NaN NoData semantics");
        }
    }
    if (!(stream >> token) || token != "END") {
        throw Exception(DT_E_CORRUPTED_DATA, "missing DGRID END marker");
    }
    return grid;
}

dt_contour_info ContourSet::info() const {
    dt_contour_info result{};
    result.struct_size = sizeof(result);
    result.line_count = lines.size();
    if (lines.empty()) return result;
    result.minimum_elevation = std::numeric_limits<double>::infinity();
    result.maximum_elevation = -result.minimum_elevation;
    bool first = true;
    for (const auto& line : lines) {
        result.vertex_count += line.points.size();
        result.minimum_elevation =
            std::min(result.minimum_elevation, line.elevation);
        result.maximum_elevation =
            std::max(result.maximum_elevation, line.elevation);
        for (const auto& point : line.points) {
            if (first) {
                result.bounds = {point.x, point.y, point.x, point.y};
                first = false;
            } else {
                result.bounds.xmin = std::min(result.bounds.xmin, point.x);
                result.bounds.ymin = std::min(result.bounds.ymin, point.y);
                result.bounds.xmax = std::max(result.bounds.xmax, point.x);
                result.bounds.ymax = std::max(result.bounds.ymax, point.y);
            }
        }
    }
    return result;
}

void ContourSet::save_text(const char* file_name) const {
    require_file_name(file_name);
    std::ofstream stream(std::filesystem::u8path(file_name),
                         std::ios::binary | std::ios::trunc);
    if (!stream) throw Exception(DT_E_IO, "cannot open contour file for writing");
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << "DCONTOUR 1\nLINES " << lines.size()
           << '\n';
    for (const auto& line : lines) {
        stream << "LINE " << line.elevation << ' ' << line.flags << ' '
               << line.points.size() << '\n';
        for (const auto& point : line.points) {
            stream << point.x << ' ' << point.y << ' ' << point.z << '\n';
        }
    }
    stream << "END\n";
    if (!stream) throw Exception(DT_E_IO, "failed while writing contour file");
}

std::unique_ptr<ContourSet> ContourSet::load_text(const char* file_name) {
    require_file_name(file_name);
    std::ifstream stream(std::filesystem::u8path(file_name), std::ios::binary);
    if (!stream) throw Exception(DT_E_IO, "cannot open contour file");
    stream.imbue(std::locale::classic());
    std::string token;
    uint32_t version = 0;
    uint64_t line_count = 0;
    if (!(stream >> token >> version) || token != "DCONTOUR" || version != 1 ||
        !(stream >> token >> line_count) || token != "LINES") {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DCONTOUR header");
    }
    if (line_count > kMaximumContourLines ||
        line_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "too many DCONTOUR lines");
    }
    auto result = std::make_unique<ContourSet>();
    result->lines.reserve(static_cast<size_t>(line_count));
    uint64_t total_vertices = 0;
    for (uint64_t i = 0; i < line_count; ++i) {
        ContourLine line;
        uint64_t point_count = 0;
        if (!(stream >> token >> line.elevation >> line.flags >> point_count) ||
            token != "LINE" || !finite(line.elevation) || point_count < 2 ||
            point_count > kMaximumContourVertices - total_vertices ||
            point_count >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw Exception(DT_E_CORRUPTED_DATA, "invalid DCONTOUR line");
        }
        total_vertices += point_count;
        line.points.resize(static_cast<size_t>(point_count));
        for (auto& point : line.points) {
            if (!(stream >> point.x >> point.y >> point.z) || !finite(point.x) ||
                !finite(point.y) || !finite(point.z)) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "invalid DCONTOUR vertex");
            }
        }
        result->lines.push_back(std::move(line));
    }
    if (!(stream >> token) || token != "END") {
        throw Exception(DT_E_CORRUPTED_DATA, "missing DCONTOUR END marker");
    }
    return result;
}

std::unique_ptr<Grid> grid_from_tin(Context& tin,
                                    const dt_tin_to_grid_options& options,
                                    const ProgressCallback& progress,
                                    const CancelCallback& cancelled) {
    const auto statistics = tin.statistics();
    if (statistics.dimension != 2) {
        throw Exception(DT_E_EMPTY, "TIN has no two-dimensional surface");
    }
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = options.flags | DT_GRID_HAS_NODATA;
    create.width = options.width;
    create.height = options.height;
    std::copy(std::begin(options.geo_transform), std::end(options.geo_transform),
              create.geo_transform);
    create.nodata_value = options.nodata_value;
    auto grid = std::make_unique<Grid>(create);
    grid->set_crs_wkt(tin.crs_wkt());
    std::vector<double> row(static_cast<size_t>(create.width),
                            create.nodata_value);
    for (uint64_t y = 0; y < create.height; ++y) {
        check_cancelled(cancelled);
        for (uint64_t x = 0; x < create.width; ++x) {
            const auto sample = grid->point(x, y, 0.0);
            const auto location = tin.locate(sample);
            switch (location.type) {
            case DT_LOCATION_VERTEX:
                row[static_cast<size_t>(x)] = location.vertex.point.z;
                break;
            case DT_LOCATION_EDGE:
                if (location.edge.vertex[0].id != 0 &&
                    location.edge.vertex[1].id != 0) {
                    row[static_cast<size_t>(x)] =
                        interpolate_edge_z(location.edge, sample.x, sample.y);
                }
                break;
            case DT_LOCATION_FACE:
                row[static_cast<size_t>(x)] =
                    interpolate_triangle_z(location.triangle, sample.x, sample.y);
                break;
            default:
                break;
            }
        }
        grid->write_window(0, y, create.width, 1, row.data(), create.width);
        std::fill(row.begin(), row.end(), create.nodata_value);
        report_progress(progress,
                        static_cast<double>(y + 1) /
                            static_cast<double>(create.height));
    }
    return grid;
}

std::unique_ptr<Grid> grid_from_cdt(CdtContext& cdt,
                                    const dt_tin_to_grid_options& options,
                                    const ProgressCallback& progress,
                                    const CancelCallback& cancelled) {
    const auto statistics = cdt.statistics();
    if (statistics.domain_triangle_count == 0) {
        throw Exception(DT_E_EMPTY, "CDT has no active domain surface");
    }
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = options.flags | DT_GRID_HAS_NODATA;
    create.width = options.width;
    create.height = options.height;
    std::copy(std::begin(options.geo_transform), std::end(options.geo_transform),
              create.geo_transform);
    create.nodata_value = options.nodata_value;
    auto grid = std::make_unique<Grid>(create);
    grid->set_crs_wkt(cdt.crs_wkt());
    std::vector<double> row(static_cast<size_t>(create.width),
                            create.nodata_value);
    for (uint64_t y = 0; y < create.height; ++y) {
        check_cancelled(cancelled);
        for (uint64_t x = 0; x < create.width; ++x) {
            const auto sample = grid->point(x, y, 0.0);
            try {
                row[static_cast<size_t>(x)] = cdt.sample_height_xy(sample);
            } catch (const Exception& error) {
                if (error.status() != DT_E_NOT_FOUND) throw;
            }
        }
        grid->write_window(0, y, create.width, 1, row.data(), create.width);
        std::fill(row.begin(), row.end(), create.nodata_value);
        report_progress(progress,
                        static_cast<double>(y + 1) /
                            static_cast<double>(create.height));
    }
    return grid;
}

std::vector<dt_point3> points_from_grid(
    const Grid& grid, const dt_grid_to_tin_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    std::vector<dt_point3> points;
    const auto& values = grid.values();
    points.reserve(values.size());
    bool found_nodata = false;
    for (uint64_t row = 0; row < grid.height(); ++row) {
        check_cancelled(cancelled);
        for (uint64_t column = 0; column < grid.width(); ++column) {
            const double value =
                values[static_cast<size_t>(row * grid.width() + column)];
            if (grid.is_nodata(value)) {
                found_nodata = true;
                continue;
            }
            if (!finite(value)) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "grid contains a non-finite elevation");
            }
            points.push_back(grid.point(column, row, value));
        }
        report_progress(progress,
                        0.5 * static_cast<double>(row + 1) /
                            static_cast<double>(grid.height()));
    }
    if (found_nodata &&
        (options.flags & DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING) == 0) {
        throw Exception(
            DT_E_UNSUPPORTED,
            "GRID contains NoData nodes; enable bridging explicitly or use CDT hole boundaries");
    }
    if (points.size() < 3) {
        throw Exception(DT_E_EMPTY, "GRID has fewer than three valid nodes");
    }
    return points;
}

std::vector<dt_point3> points_from_contours(
    const ContourSet& contours,
    const dt_contours_to_tin_options& options) {
    if (options.flags != 0 || !finite(options.maximum_segment_length) ||
        options.maximum_segment_length < 0.0 ||
        !finite(options.merge_tolerance) || options.merge_tolerance < 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "invalid contour-to-TIN options");
    }
    if (contours.lines.empty()) {
        throw Exception(DT_E_EMPTY, "contour set is empty");
    }

    const auto metadata = contours.info();
    if (metadata.vertex_count == 0) {
        throw Exception(DT_E_EMPTY, "contour set contains no vertices");
    }
    if (options.merge_tolerance > 0.0) {
        const double extent = std::max(metadata.bounds.xmax - metadata.bounds.xmin,
                                       metadata.bounds.ymax - metadata.bounds.ymin);
        validate_stitch_scale(extent, options.merge_tolerance);
    }

    std::vector<dt_point3> points;
    points.reserve(static_cast<size_t>(std::min<uint64_t>(
        metadata.vertex_count, kMaximumContourVertices)));
    std::unordered_map<ExactPointKey, size_t, ExactPointKeyHash> exact;
    std::unordered_map<PointKey, std::vector<size_t>, PointKeyHash> cells;
    const double tolerance_squared =
        options.merge_tolerance * options.merge_tolerance;

    auto elevations_agree = [](double a, double b) {
        const double scale = std::max({1.0, std::abs(a), std::abs(b)});
        return std::abs(a - b) <= scale * 1.0e-10;
    };
    auto append_point = [&](dt_point3 point) {
        point.x = point.x == 0.0 ? 0.0 : point.x;
        point.y = point.y == 0.0 ? 0.0 : point.y;
        if (!finite(point.x) || !finite(point.y) || !finite(point.z)) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "contour contains a non-finite sample");
        }

        if (options.merge_tolerance == 0.0) {
            const ExactPointKey key{point.x, point.y};
            const auto found = exact.find(key);
            if (found != exact.end()) {
                if (!elevations_agree(points[found->second].z, point.z)) {
                    throw Exception(DT_E_CORRUPTED_DATA,
                                    "duplicate contour XY has conflicting elevations");
                }
                return;
            }
            if (points.size() >= kMaximumContourVertices) {
                throw Exception(DT_E_LIMIT_EXCEEDED,
                                "too many contour-to-TIN samples");
            }
            exact.emplace(key, points.size());
            points.push_back(point);
            return;
        }

        const PointKey key = point_key(point, metadata.bounds.xmin,
                                       metadata.bounds.ymin,
                                       options.merge_tolerance);
        bool matched = false;
        for (int64_t dy = -1; dy <= 1; ++dy) {
            for (int64_t dx = -1; dx <= 1; ++dx) {
                const auto found = cells.find({key.x + dx, key.y + dy});
                if (found == cells.end()) continue;
                for (size_t index : found->second) {
                    const double x = points[index].x - point.x;
                    const double y = points[index].y - point.y;
                    if (x * x + y * y > tolerance_squared) continue;
                    if (!elevations_agree(points[index].z, point.z)) {
                        throw Exception(DT_E_CORRUPTED_DATA,
                                        "nearby contour vertices have conflicting elevations");
                    }
                    matched = true;
                }
            }
        }
        if (matched) return;
        if (points.size() >= kMaximumContourVertices) {
            throw Exception(DT_E_LIMIT_EXCEEDED,
                            "too many contour-to-TIN samples");
        }
        cells[key].push_back(points.size());
        points.push_back(point);
    };

    auto append_segment = [&](const dt_point3& source_a,
                              const dt_point3& source_b,
                              double elevation) {
        const double dx = source_b.x - source_a.x;
        const double dy = source_b.y - source_a.y;
        const double length = std::hypot(dx, dy);
        if (!finite(length)) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "contour segment length is invalid");
        }
        uint64_t divisions = 1;
        if (options.maximum_segment_length > 0.0) {
            const double required =
                std::ceil(length / options.maximum_segment_length);
            if (!finite(required) ||
                required > static_cast<double>(kMaximumContourVertices)) {
                throw Exception(DT_E_LIMIT_EXCEEDED,
                                "contour segment creates too many samples");
            }
            divisions = std::max<uint64_t>(1, static_cast<uint64_t>(required));
        }
        for (uint64_t i = 1; i <= divisions; ++i) {
            const double t = static_cast<double>(i) /
                             static_cast<double>(divisions);
            append_point({source_a.x + t * dx, source_a.y + t * dy,
                          elevation});
        }
    };

    for (const auto& line : contours.lines) {
        if (!finite(line.elevation) || line.points.size() < 2) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "invalid contour line geometry");
        }
        append_point({line.points.front().x, line.points.front().y,
                      line.elevation});
        for (size_t i = 1; i < line.points.size(); ++i) {
            append_segment(line.points[i - 1], line.points[i],
                           line.elevation);
        }
        if ((line.flags & DT_CONTOUR_LINE_CLOSED) != 0) {
            append_segment(line.points.back(), line.points.front(),
                           line.elevation);
        }
    }

    if (points.size() < 3) {
        throw Exception(DT_E_EMPTY,
                        "contours do not provide enough unique XY samples");
    }
    const Kernel::Point_2 first(points[0].x, points[0].y);
    size_t second_index = 1;
    while (second_index < points.size() &&
           points[second_index].x == points[0].x &&
           points[second_index].y == points[0].y) {
        ++second_index;
    }
    if (second_index == points.size()) {
        throw Exception(DT_E_EMPTY, "all contour samples share one XY");
    }
    const Kernel::Point_2 second(points[second_index].x,
                                 points[second_index].y);
    bool two_dimensional = false;
    for (size_t i = second_index + 1; i < points.size(); ++i) {
        if (CGAL::orientation(first, second,
                              Kernel::Point_2(points[i].x, points[i].y)) !=
            CGAL::COLLINEAR) {
            two_dimensional = true;
            break;
        }
    }
    if (!two_dimensional) {
        throw Exception(DT_E_EMPTY,
                        "contour samples are collinear and cannot form a TIN");
    }
    return points;
}

std::unique_ptr<ContourSet> contours_from_tin(
    Context& tin, const dt_contour_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    const auto statistics = tin.statistics();
    if (statistics.dimension != 2) {
        throw Exception(DT_E_EMPTY, "TIN has no two-dimensional surface");
    }
    double xmin = std::numeric_limits<double>::infinity();
    double ymin = xmin;
    double xmax = -xmin;
    double ymax = -xmin;
    double zmin = xmin;
    double zmax = -xmin;
    uint64_t visited = 0;
    tin.visit_triangles([&](const dt_triangle3& triangle) {
        if ((visited & 0x3fffU) == 0) check_cancelled(cancelled);
        for (const auto& vertex : triangle.vertex) {
            xmin = std::min(xmin, vertex.point.x);
            ymin = std::min(ymin, vertex.point.y);
            xmax = std::max(xmax, vertex.point.x);
            ymax = std::max(ymax, vertex.point.y);
            zmin = std::min(zmin, vertex.point.z);
            zmax = std::max(zmax, vertex.point.z);
        }
        ++visited;
        if ((visited & 0x3fffU) == 0) {
            report_progress(progress,
                            0.35 * static_cast<double>(visited) /
                                static_cast<double>(statistics.finite_triangle_count));
        }
    });
    const auto levels = make_levels(zmin, zmax, options);
    auto output = std::make_unique<ContourSet>();
    output->crs_wkt = tin.crs_wkt();
    if (levels.empty()) return output;
    const double extent = std::max(xmax - xmin, ymax - ymin);
    const double tolerance = options.stitch_tolerance > 0.0
                                 ? options.stitch_tolerance
                                 : std::max(extent * 1e-10, 1e-12);
    validate_stitch_scale(extent, tolerance);
    std::vector<std::vector<Segment>> segments(levels.size());
    visited = 0;
    tin.visit_triangles([&](const dt_triangle3& triangle) {
        if ((visited & 0x3fffU) == 0) check_cancelled(cancelled);
        add_triangle_segments(triangle, levels, segments, tolerance);
        ++visited;
        if ((visited & 0x3fffU) == 0) {
            report_progress(progress,
                            0.35 + 0.5 * static_cast<double>(visited) /
                                       static_cast<double>(statistics.finite_triangle_count));
        }
    });
    for (size_t i = 0; i < levels.size(); ++i) {
        check_cancelled(cancelled);
        stitch_level(levels[i], segments[i], xmin, ymin, tolerance, *output);
        report_progress(progress,
                        0.85 + 0.15 * static_cast<double>(i + 1) /
                                   static_cast<double>(levels.size()));
    }
    return output;
}

std::unique_ptr<ContourSet> contours_from_cdt(
    CdtContext& cdt, const dt_contour_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    const auto statistics = cdt.statistics();
    if (statistics.domain_triangle_count == 0) {
        throw Exception(DT_E_EMPTY, "CDT has no active domain surface");
    }
    double xmin = std::numeric_limits<double>::infinity();
    double ymin = xmin;
    double xmax = -xmin;
    double ymax = -xmin;
    double zmin = xmin;
    double zmax = -xmin;
    uint64_t visited = 0;
    cdt.visit_domain_triangles([&](const dt_triangle3& triangle) {
        if ((visited & 0x3fffU) == 0) check_cancelled(cancelled);
        for (const auto& vertex : triangle.vertex) {
            xmin = std::min(xmin, vertex.point.x);
            ymin = std::min(ymin, vertex.point.y);
            xmax = std::max(xmax, vertex.point.x);
            ymax = std::max(ymax, vertex.point.y);
            zmin = std::min(zmin, vertex.point.z);
            zmax = std::max(zmax, vertex.point.z);
        }
        ++visited;
        if ((visited & 0x3fffU) == 0) {
            report_progress(progress,
                            0.35 * static_cast<double>(visited) /
                                static_cast<double>(statistics.domain_triangle_count));
        }
    });
    const auto levels = make_levels(zmin, zmax, options);
    auto output = std::make_unique<ContourSet>();
    output->crs_wkt = cdt.crs_wkt();
    if (levels.empty()) return output;
    const double extent = std::max(xmax - xmin, ymax - ymin);
    const double tolerance = options.stitch_tolerance > 0.0
                                 ? options.stitch_tolerance
                                 : std::max(extent * 1e-10, 1e-12);
    validate_stitch_scale(extent, tolerance);
    std::vector<std::vector<Segment>> segments(levels.size());
    visited = 0;
    cdt.visit_domain_triangles([&](const dt_triangle3& triangle) {
        if ((visited & 0x3fffU) == 0) check_cancelled(cancelled);
        add_triangle_segments(triangle, levels, segments, tolerance);
        ++visited;
        if ((visited & 0x3fffU) == 0) {
            report_progress(progress,
                            0.35 + 0.5 * static_cast<double>(visited) /
                                       static_cast<double>(statistics.domain_triangle_count));
        }
    });
    for (size_t i = 0; i < levels.size(); ++i) {
        check_cancelled(cancelled);
        stitch_level(levels[i], segments[i], xmin, ymin, tolerance, *output);
        report_progress(progress,
                        0.85 + 0.15 * static_cast<double>(i + 1) /
                                   static_cast<double>(levels.size()));
    }
    return output;
}

std::unique_ptr<ContourSet> contours_from_grid(
    const Grid& grid, const dt_contour_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    if (grid.width() < 2 || grid.height() < 2) {
        throw Exception(DT_E_EMPTY, "GRID must contain at least 2 x 2 nodes");
    }
    const auto& values = grid.values();
    auto value = [&](uint64_t column, uint64_t row) {
        return values[static_cast<size_t>(row * grid.width() + column)];
    };
    double zmin = std::numeric_limits<double>::infinity();
    double zmax = -zmin;
    uint64_t scanned = 0;
    for (double z : values) {
        if ((scanned & 0xffffU) == 0) check_cancelled(cancelled);
        if (!grid.is_nodata(z)) {
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
        ++scanned;
    }
    if (!finite(zmin)) throw Exception(DT_E_EMPTY, "GRID has no valid values");
    const auto levels = make_levels(zmin, zmax, options);
    auto output = std::make_unique<ContourSet>();
    output->crs_wkt = grid.crs_wkt();
    if (levels.empty()) return output;
    const auto grid_info = grid.info();
    const double extent = std::max(grid_info.bounds.xmax - grid_info.bounds.xmin,
                                   grid_info.bounds.ymax - grid_info.bounds.ymin);
    const double tolerance = options.stitch_tolerance > 0.0
                                 ? options.stitch_tolerance
                                 : std::max(extent * 1e-10, 1e-12);
    validate_stitch_scale(extent, tolerance);
    std::vector<std::vector<Segment>> segments(levels.size());
    bool found_cell = false;
    for (uint64_t row = 0; row + 1 < grid.height(); ++row) {
        check_cancelled(cancelled);
        for (uint64_t column = 0; column + 1 < grid.width(); ++column) {
            const double z00 = value(column, row);
            const double z10 = value(column + 1, row);
            const double z01 = value(column, row + 1);
            const double z11 = value(column + 1, row + 1);
            if (grid.is_nodata(z00) || grid.is_nodata(z10) ||
                grid.is_nodata(z01) || grid.is_nodata(z11)) {
                continue;
            }
            found_cell = true;
            const dt_point3 p00 = grid.point(column, row, z00);
            const dt_point3 p10 = grid.point(column + 1, row, z10);
            const dt_point3 p01 = grid.point(column, row + 1, z01);
            const dt_point3 p11 = grid.point(column + 1, row + 1, z11);
            const dt_triangle3 first{{{p00, 0}, {p10, 0}, {p11, 0}}};
            const dt_triangle3 second{{{p00, 0}, {p11, 0}, {p01, 0}}};
            add_triangle_segments(first, levels, segments, tolerance);
            add_triangle_segments(second, levels, segments, tolerance);
        }
        report_progress(progress,
                        0.85 * static_cast<double>(row + 1) /
                            static_cast<double>(grid.height() - 1));
    }
    if (!found_cell) {
        throw Exception(DT_E_EMPTY, "GRID has no complete valid cell");
    }
    for (size_t i = 0; i < levels.size(); ++i) {
        check_cancelled(cancelled);
        stitch_level(levels[i], segments[i], grid_info.bounds.xmin,
                     grid_info.bounds.ymin, tolerance, *output);
        report_progress(progress,
                        0.85 + 0.15 * static_cast<double>(i + 1) /
                                   static_cast<double>(levels.size()));
    }
    return output;
}

} // namespace dt
