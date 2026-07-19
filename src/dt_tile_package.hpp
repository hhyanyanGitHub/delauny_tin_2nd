#ifndef DT_TILE_PACKAGE_HPP
#define DT_TILE_PACKAGE_HPP

#include "dt_task_api.h"
#include "dt_terrain_core.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace dt {

struct PersistentTileKey {
    uint64_t scale = 1;
    uint64_t x = 0;
    uint64_t y = 0;
    uint32_t method = 0;
    uint32_t flags = 0;

    bool operator==(const PersistentTileKey& other) const noexcept {
        return scale == other.scale && x == other.x && y == other.y &&
               method == other.method && flags == other.flags;
    }
};

struct PersistentTileDescriptor {
    PersistentTileKey key{};
    uint64_t payload_offset = 0;
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t payload_hash = 0;
    dt_grid_overview_result overview{};
};

class GridTilePackage final {
public:
    GridTilePackage(const Grid& source, uint32_t tile_width,
                    uint32_t tile_height,
                    const dt_grid_view_disk_cache_options& options);
    ~GridTilePackage();

    GridTilePackage(const GridTilePackage&) = delete;
    GridTilePackage& operator=(const GridTilePackage&) = delete;

    std::optional<PersistentTileDescriptor> find(
        const PersistentTileKey& key);
    /* Returns false when an opted-in writable package discarded a corrupt
       lazy payload and the caller should regenerate it from the source. */
    bool load(const PersistentTileDescriptor& descriptor,
              std::vector<double>& values,
              dt_grid_overview_result& overview) const;
    bool append(const PersistentTileKey& key, uint64_t width, uint64_t height,
                const std::vector<double>& values,
                const dt_grid_overview_result& overview) noexcept;
    dt_grid_view_disk_cache_statistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

uint64_t grid_tile_source_fingerprint(const Grid& source,
                                      uint64_t source_revision);

} // namespace dt

#endif
