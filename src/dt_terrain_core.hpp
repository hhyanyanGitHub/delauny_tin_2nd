#ifndef DT_TERRAIN_CORE_HPP
#define DT_TERRAIN_CORE_HPP

#include "dt_core.hpp"
#include "dt_terrain_api.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dt {

using ProgressCallback = std::function<void(double)>;
using CancelCallback = std::function<bool()>;

class Grid final {
public:
    explicit Grid(const dt_grid_create_options& options);

    dt_grid_info info() const;
    void read_window(uint64_t column, uint64_t row, uint64_t width,
                     uint64_t height, double* output, uint64_t stride) const;
    void write_window(uint64_t column, uint64_t row, uint64_t width,
                      uint64_t height, const double* input, uint64_t stride);
    void save_text(const char* file_name) const;
    static std::unique_ptr<Grid> load_text(const char* file_name);

    uint64_t width() const noexcept { return options_.width; }
    uint64_t height() const noexcept { return options_.height; }
    uint32_t flags() const noexcept { return options_.flags; }
    double nodata() const noexcept { return options_.nodata_value; }
    const double* transform() const noexcept { return options_.geo_transform; }
    const std::vector<double>& values() const noexcept { return values_; }
    bool is_nodata(double value) const noexcept;
    dt_point3 point(uint64_t column, uint64_t row, double z) const noexcept;

private:
    dt_grid_create_options options_{};
    std::vector<double> values_;
    uint64_t generation_ = 1;

    size_t offset(uint64_t column, uint64_t row) const;
    void validate_window(uint64_t column, uint64_t row, uint64_t width,
                         uint64_t height, uint64_t stride) const;
};

struct ContourLine {
    double elevation = 0.0;
    uint32_t flags = 0;
    std::vector<dt_point3> points;
};

class ContourSet final {
public:
    std::vector<ContourLine> lines;

    dt_contour_info info() const;
    void save_text(const char* file_name) const;
    static std::unique_ptr<ContourSet> load_text(const char* file_name);
};

std::unique_ptr<Grid> grid_from_tin(Context& tin,
                                    const dt_tin_to_grid_options& options,
                                    const ProgressCallback& progress = {},
                                    const CancelCallback& cancelled = {});
std::vector<dt_point3> points_from_grid(
    const Grid& grid, const dt_grid_to_tin_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<ContourSet> contours_from_tin(
    Context& tin, const dt_contour_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<ContourSet> contours_from_grid(
    const Grid& grid, const dt_contour_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});

} // namespace dt

#endif
