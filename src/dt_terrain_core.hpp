#ifndef DT_TERRAIN_CORE_HPP
#define DT_TERRAIN_CORE_HPP

#include "dt_core.hpp"
#include "dt_grid_storage.hpp"
#include "dt_terrain_api.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dt {

class CdtContext;
struct GridEarthworkComputation;

using ProgressCallback = std::function<void(double)>;
using CancelCallback = std::function<bool()>;

class Grid final {
public:
    explicit Grid(const dt_grid_create_options& options,
                  bool initialize_storage = true);

    dt_grid_info info() const;
    void read_window(uint64_t column, uint64_t row, uint64_t width,
                     uint64_t height, double* output, uint64_t stride) const;
    void prefetch_window(uint64_t column, uint64_t row, uint64_t width,
                         uint64_t height) const;
    dt_grid_verify_result verify_window(
        uint64_t column, uint64_t row, uint64_t width, uint64_t height,
        const ProgressCallback& progress = {},
        const CancelCallback& cancelled = {}) const;
    dt_grid_overview_result read_overview(
        const dt_grid_overview_options& options, uint64_t output_width,
        uint64_t output_height, double* output, uint64_t stride,
        const ProgressCallback& progress = {},
        const CancelCallback& cancelled = {}) const;
    dt_grid_window view_window(const dt_grid_view_options& options) const;
    void write_window(uint64_t column, uint64_t row, uint64_t width,
                      uint64_t height, const double* input, uint64_t stride);
    void save_text(const char* file_name) const;
    static std::unique_ptr<Grid> load_text(const char* file_name);
    void save_binary(const char* file_name);
    static std::unique_ptr<Grid> load_binary(const char* file_name);
    static void verify_binary_file(const char* file_name);

    uint64_t width() const noexcept { return options_.width; }
    uint64_t height() const noexcept { return options_.height; }
    uint32_t flags() const noexcept { return options_.flags; }
    double nodata() const noexcept { return options_.nodata_value; }
    const double* transform() const noexcept { return options_.geo_transform; }
    const GridStorage& values() const noexcept { return values_; }
    const std::string& crs_wkt() const noexcept { return crs_wkt_; }
    void set_crs_wkt(std::string crs_wkt) { crs_wkt_ = std::move(crs_wkt); }
    bool is_nodata(double value) const noexcept;
    dt_point3 point(uint64_t column, uint64_t row, double z) const noexcept;
    dt_surface_analysis analyze_surface_xy(const dt_point3& query) const;

private:
    friend std::unique_ptr<Grid> grid_derive_terrain(
        const Grid& source, const dt_grid_terrain_options& options,
        const ProgressCallback& progress, const CancelCallback& cancelled);
    friend GridEarthworkComputation grid_compare_earthwork(
        const Grid& existing, const Grid& design,
        const dt_grid_earthwork_options& options,
        const ProgressCallback& progress, const CancelCallback& cancelled);
    friend std::unique_ptr<Grid> grid_resample_like(
        const Grid& source, const Grid& reference,
        const dt_grid_resample_options& options,
        const ProgressCallback& progress, const CancelCallback& cancelled);
    friend std::unique_ptr<Grid> grid_clip_polygon(
        const Grid& source, const std::vector<dt_point3>& polygon,
        const dt_grid_clip_options& options,
        const ProgressCallback& progress, const CancelCallback& cancelled);

    dt_grid_create_options options_{};
    GridStorage values_;
    std::string crs_wkt_;
    uint64_t generation_ = 1;
    std::vector<double> persistent_overview_;
    uint64_t persistent_overview_width_ = 0;
    uint64_t persistent_overview_height_ = 0;
    dt_grid_overview_result persistent_overview_result_{};
    struct PyramidLevel {
        uint32_t scale = 0;
        uint64_t width = 0;
        uint64_t height = 0;
        std::unique_ptr<GridStorage> values;
    };
    std::vector<PyramidLevel> pyramid_;
    std::vector<uint64_t> binary_checksums_;
    uint64_t binary_checksum_block_bytes_ = 0;
    /* 0 unknown, 1 verified, 2 failed. Guarded independently so concurrent
       read-only viewport tasks may share the cache. */
    mutable std::mutex binary_verification_mutex_;
    mutable std::vector<uint8_t> binary_verification_state_;
    bool binary_valid_count_available_ = false;
    uint64_t binary_valid_count_ = 0;

    size_t offset(uint64_t column, uint64_t row) const;
    void validate_window(uint64_t column, uint64_t row, uint64_t width,
                         uint64_t height, uint64_t stride) const;
};

struct GridEarthworkComputation {
    dt_grid_earthwork_result result{};
    std::unique_ptr<Grid> difference_grid;
};

struct ContourLine {
    double elevation = 0.0;
    uint32_t flags = 0;
    std::vector<dt_point3> points;
};

class ContourSet final {
public:
    std::vector<ContourLine> lines;
    std::string crs_wkt;

    dt_contour_info info() const;
    void save_text(const char* file_name) const;
    static std::unique_ptr<ContourSet> load_text(const char* file_name);
};

std::unique_ptr<Grid> grid_from_tin(Context& tin,
                                    const dt_tin_to_grid_options& options,
                                    const ProgressCallback& progress = {},
                                    const CancelCallback& cancelled = {});
std::unique_ptr<Grid> grid_from_cdt(CdtContext& cdt,
                                    const dt_tin_to_grid_options& options,
                                    const ProgressCallback& progress = {},
                                    const CancelCallback& cancelled = {});
std::unique_ptr<Grid> grid_derive_terrain(
    const Grid& source, const dt_grid_terrain_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
GridEarthworkComputation grid_compare_earthwork(
    const Grid& existing, const Grid& design,
    const dt_grid_earthwork_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<Grid> grid_resample_like(
    const Grid& source, const Grid& reference,
    const dt_grid_resample_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<Grid> grid_clip_polygon(
    const Grid& source, const std::vector<dt_point3>& polygon,
    const dt_grid_clip_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::vector<dt_point3> points_from_grid(
    const Grid& grid, const dt_grid_to_tin_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::vector<dt_point3> points_from_contours(
    const ContourSet& contours,
    const dt_contours_to_tin_options& options);
std::unique_ptr<ContourSet> contours_from_tin(
    Context& tin, const dt_contour_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<ContourSet> contours_from_cdt(
    CdtContext& cdt, const dt_contour_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});
std::unique_ptr<ContourSet> contours_from_grid(
    const Grid& grid, const dt_contour_options& options,
    const ProgressCallback& progress = {},
    const CancelCallback& cancelled = {});

} // namespace dt

#endif
