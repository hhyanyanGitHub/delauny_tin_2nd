#ifndef DT_VIEW_CACHE_HPP
#define DT_VIEW_CACHE_HPP

#include "dt_task_api.h"
#include "dt_terrain_core.hpp"

#include <memory>
#include <vector>

namespace dt {

struct CachedGridView {
    dt_grid_window source_window{};
    std::vector<double> values;
    dt_grid_overview_result overview{};
    dt_grid_verify_result verification{};
    uint32_t flags = 0;
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t lod_scale = 1;
    uint64_t tile_count = 0;
    uint64_t reused_tile_count = 0;
};

class GridViewCache final {
public:
    GridViewCache(std::shared_ptr<Grid> source,
                  const dt_grid_view_cache_options& options);
    GridViewCache(std::shared_ptr<Grid> source,
                  const dt_grid_view_cache_options& options,
                  const dt_grid_view_disk_cache_options& disk_options);
    ~GridViewCache();

    GridViewCache(const GridViewCache&) = delete;
    GridViewCache& operator=(const GridViewCache&) = delete;

    CachedGridView read_view(const dt_grid_view_request_options& options,
                             const ProgressCallback& progress,
                             const CancelCallback& cancelled,
                             uint64_t forced_lod_scale = 0);
    uint64_t recommended_lod_scale(
        const dt_grid_view_request_options& options) const;
    dt_grid_view_cache_statistics statistics() const;
    dt_grid_view_disk_cache_statistics disk_statistics() const;
    dt_grid_view_cache_compact_result compact();
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dt

#endif
