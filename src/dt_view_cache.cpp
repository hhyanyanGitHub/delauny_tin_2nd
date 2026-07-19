#include "dt_view_cache.hpp"

#include "dt_tile_package.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace dt {
namespace {

constexpr uint32_t kDefaultTileSize = 128;
constexpr uint64_t kDefaultMaximumBytes = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDefaultMaximumTiles = 4096;
constexpr uint64_t kMaximumRequestTiles = 1000000;

bool finished_state(uint8_t state) noexcept { return state >= 2; }

uint64_t nearest_index(uint64_t index, uint64_t source_extent,
                       uint64_t output_extent) {
    return ((2U * index + 1U) * source_extent) / (2U * output_extent);
}

} // namespace

struct GridViewCache::Impl {
    struct Key {
        uint64_t generation = 0;
        uint64_t scale = 1;
        uint64_t x = 0;
        uint64_t y = 0;
        uint32_t method = 0;
        uint32_t flags = 0;

        bool operator==(const Key& other) const noexcept {
            return generation == other.generation && scale == other.scale &&
                   x == other.x && y == other.y && method == other.method &&
                   flags == other.flags;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            size_t value = static_cast<size_t>(key.generation);
            const auto mix = [&](uint64_t item) {
                value ^= static_cast<size_t>(item) +
                         static_cast<size_t>(0x9e3779b97f4a7c15ULL) +
                         (value << 6U) + (value >> 2U);
            };
            mix(key.scale);
            mix(key.x);
            mix(key.y);
            mix(key.method);
            mix(key.flags);
            return value;
        }
    };

    enum EntryState : uint8_t { Queued = 0, Loading = 1, Ready = 2, Failed = 3 };

    struct Entry {
        Key key{};
        EntryState state = Queued;
        uint64_t queue_version = 1;
        long double priority = 0.0L;
        uint64_t stamp = 0;
        uint64_t demand = 0;
        uint64_t source_column = 0;
        uint64_t source_row = 0;
        uint64_t source_width = 0;
        uint64_t source_height = 0;
        uint64_t width = 0;
        uint64_t height = 0;
        std::vector<double> values;
        dt_grid_overview_result overview{};
        std::optional<PersistentTileDescriptor> disk_descriptor;
        bool loaded_from_disk = false;
        bool recovered_disk_payload = false;
        dt_status failure_status = DT_OK;
        std::string failure_message;
        std::condition_variable changed;
    };

    struct Job {
        long double priority = 0.0L;
        uint64_t sequence = 0;
        uint64_t version = 0;
        std::shared_ptr<Entry> entry;
    };

    struct JobOrder {
        bool operator()(const Job& a, const Job& b) const noexcept {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.sequence > b.sequence;
        }
    };

    enum class Reuse { Miss, Hit, Coalesced, Disk };
    struct Lease {
        std::shared_ptr<Entry> entry;
        Reuse reuse = Reuse::Miss;
    };

    std::shared_ptr<Grid> source;
    std::unique_ptr<GridTilePackage> disk_package;
    uint64_t persistent_generation = 0;
    uint32_t tile_width = kDefaultTileSize;
    uint32_t tile_height = kDefaultTileSize;
    uint64_t maximum_bytes = kDefaultMaximumBytes;
    uint64_t maximum_tiles = kDefaultMaximumTiles;
    mutable std::mutex mutex;
    std::condition_variable jobs_changed;
    std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> entries;
    std::priority_queue<Job, std::vector<Job>, JobOrder> jobs;
    std::vector<std::thread> workers;
    bool stopping = false;
    uint64_t sequence = 0;
    uint64_t stamp = 0;
    uint64_t cached_bytes = 0;
    uint64_t cached_tiles = 0;
    uint64_t in_flight_tiles = 0;
    uint64_t request_count = 0;
    uint64_t hit_tile_count = 0;
    uint64_t miss_tile_count = 0;
    uint64_t coalesced_tile_count = 0;
    uint64_t eviction_count = 0;

    Impl(std::shared_ptr<Grid> source_grid,
         const dt_grid_view_cache_options& options,
         const dt_grid_view_disk_cache_options* disk_options = nullptr)
        : source(std::move(source_grid)) {
        if (!source) {
            throw Exception(DT_E_NOT_INITIALIZED,
                            "GRID view cache source is null");
        }
        if (options.flags != 0) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "unknown GRID view cache flags");
        }
        tile_width = options.tile_width == 0 ? kDefaultTileSize
                                             : options.tile_width;
        tile_height = options.tile_height == 0 ? kDefaultTileSize
                                               : options.tile_height;
        if (tile_width < 16 || tile_height < 16 || tile_width > 1024 ||
            tile_height > 1024) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "GRID view cache tile dimensions must be 16..1024");
        }
        maximum_bytes = options.maximum_bytes == 0
                            ? kDefaultMaximumBytes
                            : options.maximum_bytes;
        maximum_tiles = options.maximum_tiles == 0
                            ? kDefaultMaximumTiles
                            : options.maximum_tiles;
        const uint64_t minimum_tile_bytes =
            static_cast<uint64_t>(tile_width) * tile_height * sizeof(double);
        if (maximum_bytes < minimum_tile_bytes ||
            maximum_bytes > 8ULL * 1024ULL * 1024ULL * 1024ULL) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "GRID view cache byte capacity is invalid");
        }
        if (maximum_tiles == 0 || maximum_tiles > kMaximumRequestTiles) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "GRID view cache tile capacity is invalid");
        }
        uint32_t worker_count = options.worker_count;
        if (worker_count == 0) {
            worker_count = std::thread::hardware_concurrency();
            if (worker_count == 0) worker_count = 4;
            worker_count = std::min(worker_count, 8U);
        }
        if (worker_count > 64) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "GRID view cache worker_count exceeds 64");
        }
        if (disk_options) {
            persistent_generation = source->info().generation;
            disk_package = std::make_unique<GridTilePackage>(
                *source, tile_width, tile_height, *disk_options);
        }
        workers.reserve(worker_count);
        try {
            for (uint32_t index = 0; index < worker_count; ++index) {
                workers.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
            }
            jobs_changed.notify_all();
            for (auto& worker : workers)
                if (worker.joinable()) worker.join();
            throw;
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        jobs_changed.notify_all();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    void worker_loop() {
        for (;;) {
            std::shared_ptr<Entry> entry;
            {
                std::unique_lock<std::mutex> lock(mutex);
                jobs_changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) return;
                Job job = jobs.top();
                jobs.pop();
                entry = std::move(job.entry);
                if (!entry || entry->state != Queued ||
                    entry->queue_version != job.version) {
                    continue;
                }
                if (entry->demand == 0) {
                    entry->state = Failed;
                    entry->failure_status = DT_E_CANCELLED;
                    entry->failure_message = "unused GRID view tile was dropped";
                    --in_flight_tiles;
                    const auto found = entries.find(entry->key);
                    if (found != entries.end() && found->second == entry)
                        entries.erase(found);
                    entry->changed.notify_all();
                    continue;
                }
                entry->state = Loading;
            }

            std::vector<double> values;
            dt_grid_overview_result overview{};
            dt_status failure_status = DT_OK;
            std::string failure_message;
            bool loaded_from_disk = false;
            bool recovered_disk_payload = false;
            try {
                if (entry->disk_descriptor) {
                    loaded_from_disk = disk_package->load(
                        *entry->disk_descriptor, values, overview);
                    recovered_disk_payload = !loaded_from_disk;
                }
                if (!loaded_from_disk) {
                    values.resize(static_cast<size_t>(entry->width *
                                                      entry->height));
                    dt_grid_overview_options options{};
                    options.struct_size = sizeof(options);
                    options.method = entry->key.method;
                    options.flags = entry->key.flags;
                    options.worker_count = 1;
                    options.source_column = entry->source_column;
                    options.source_row = entry->source_row;
                    options.source_width = entry->source_width;
                    options.source_height = entry->source_height;
                    overview = source->read_overview(
                        options, entry->width, entry->height, values.data(),
                        entry->width);
                    if (disk_package &&
                        entry->key.generation == persistent_generation) {
                        const PersistentTileKey persistent_key{
                            entry->key.scale, entry->key.x, entry->key.y,
                            entry->key.method, entry->key.flags};
                        disk_package->append(persistent_key, entry->width,
                                             entry->height, values, overview);
                    }
                }
            } catch (const Exception& error) {
                failure_status = error.status();
                failure_message = error.what();
            } catch (const std::bad_alloc&) {
                failure_status = DT_E_OUT_OF_MEMORY;
                failure_message = "GRID view tile allocation failed";
            } catch (const std::exception& error) {
                failure_status = DT_E_INTERNAL;
                failure_message = error.what();
            } catch (...) {
                failure_status = DT_E_INTERNAL;
                failure_message = "unknown GRID view tile error";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                --in_flight_tiles;
                if (failure_status == DT_OK) {
                    entry->values = std::move(values);
                    entry->overview = overview;
                    entry->loaded_from_disk = loaded_from_disk;
                    entry->recovered_disk_payload = recovered_disk_payload;
                    entry->state = Ready;
                    entry->stamp = ++stamp;
                    cached_bytes += entry->values.size() * sizeof(double);
                    ++cached_tiles;
                    evict_locked();
                } else {
                    entry->state = Failed;
                    entry->failure_status = failure_status;
                    entry->failure_message = std::move(failure_message);
                }
            }
            entry->changed.notify_all();
        }
    }

    void evict_locked() {
        while (cached_bytes > maximum_bytes || cached_tiles > maximum_tiles) {
            auto victim = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                const auto& candidate = *it->second;
                if (candidate.state != Ready || candidate.demand != 0) continue;
                if (victim == entries.end() ||
                    candidate.stamp < victim->second->stamp) {
                    victim = it;
                }
            }
            if (victim == entries.end()) break;
            cached_bytes -= victim->second->values.size() * sizeof(double);
            --cached_tiles;
            ++eviction_count;
            entries.erase(victim);
        }
    }

    Lease acquire(const Key& key, long double priority) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = entries.find(key);
        if (found != entries.end() && found->second->state == Failed &&
            found->second->demand == 0) {
            entries.erase(found);
            found = entries.end();
        }
        if (found != entries.end()) {
            auto entry = found->second;
            ++entry->demand;
            if (entry->state == Ready) {
                entry->stamp = ++stamp;
                ++hit_tile_count;
                return {std::move(entry), Reuse::Hit};
            }
            if (entry->state == Queued && priority < entry->priority) {
                entry->priority = priority;
                ++entry->queue_version;
                jobs.push({priority, ++sequence, entry->queue_version, entry});
                jobs_changed.notify_one();
            }
            ++coalesced_tile_count;
            return {std::move(entry), Reuse::Coalesced};
        }

        auto entry = std::make_shared<Entry>();
        entry->key = key;
        entry->priority = priority;
        entry->demand = 1;
        const uint64_t source_columns_per_tile =
            static_cast<uint64_t>(tile_width) * key.scale;
        const uint64_t source_rows_per_tile =
            static_cast<uint64_t>(tile_height) * key.scale;
        entry->source_column = key.x * source_columns_per_tile;
        entry->source_row = key.y * source_rows_per_tile;
        entry->source_width = std::min(source_columns_per_tile,
                                       source->width() - entry->source_column);
        entry->source_height = std::min(source_rows_per_tile,
                                        source->height() - entry->source_row);
        entry->width = (entry->source_width + key.scale - 1) / key.scale;
        entry->height = (entry->source_height + key.scale - 1) / key.scale;
        Reuse reuse = Reuse::Miss;
        if (disk_package && key.generation == persistent_generation) {
            const PersistentTileKey persistent_key{
                key.scale, key.x, key.y, key.method, key.flags};
            entry->disk_descriptor = disk_package->find(persistent_key);
            if (entry->disk_descriptor) {
                if (entry->disk_descriptor->width != entry->width ||
                    entry->disk_descriptor->height != entry->height) {
                    throw Exception(DT_E_CORRUPTED_DATA,
                                    "DGTILE tile dimensions do not match GRID");
                }
                reuse = Reuse::Disk;
            }
        }
        entries.emplace(key, entry);
        jobs.push({priority, ++sequence, entry->queue_version, entry});
        ++in_flight_tiles;
        if (reuse == Reuse::Miss) ++miss_tile_count;
        jobs_changed.notify_one();
        return {std::move(entry), reuse};
    }

    void release(const std::shared_ptr<Entry>& entry) {
        if (!entry) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (entry->demand != 0) --entry->demand;
        if (entry->demand == 0 && entry->state == Failed) {
            const auto found = entries.find(entry->key);
            if (found != entries.end() && found->second == entry)
                entries.erase(found);
        }
        evict_locked();
    }

    void wait_ready(const std::shared_ptr<Entry>& entry,
                    const CancelCallback& cancelled) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!finished_state(entry->state)) {
            if (cancelled && cancelled()) {
                throw Exception(DT_E_CANCELLED,
                                "terrain operation was cancelled");
            }
            entry->changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (entry->state == Failed) {
            throw Exception(entry->failure_status,
                            entry->failure_message.empty()
                                ? "GRID view tile failed"
                                : entry->failure_message);
        }
        entry->stamp = ++stamp;
    }

    CachedGridView read_view(const dt_grid_view_request_options& request,
                             const ProgressCallback& progress,
                             const CancelCallback& cancelled) {
        constexpr uint32_t known_flags =
            DT_GRID_VIEW_REQUEST_STRICT_NODATA |
            DT_GRID_VIEW_REQUEST_USE_PYRAMID |
            DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE |
            DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS;
        if ((request.flags & ~known_flags) != 0) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "unknown GRID view request flags");
        }
        if (request.output_width == 0 || request.output_height == 0 ||
            request.output_width > 1024ULL * 1024ULL ||
            request.output_height > 1024ULL * 1024ULL ||
            request.output_width >
                1000000000ULL / request.output_height) {
            throw Exception(DT_E_LIMIT_EXCEEDED,
                            "GRID view output size is invalid or too large");
        }
        const uint32_t method = request.overview_method == 0
            ? static_cast<uint32_t>(DT_GRID_OVERVIEW_AVERAGE)
            : static_cast<uint32_t>(request.overview_method);
        if (method != DT_GRID_OVERVIEW_AVERAGE &&
            method != DT_GRID_OVERVIEW_NEAREST &&
            method != DT_GRID_OVERVIEW_MINIMUM &&
            method != DT_GRID_OVERVIEW_MAXIMUM) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "unknown GRID overview method");
        }

        dt_grid_view_options view{};
        view.struct_size = sizeof(view);
        view.world_bounds = request.world_bounds;
        view.padding_nodes = request.padding_nodes;
        CachedGridView result;
        result.source_window = source->view_window(view);
        result.width = request.output_width;
        result.height = request.output_height;
        result.flags = DT_GRID_VIEW_RESULT_USED_TILE_CACHE;
        result.verification.struct_size = sizeof(result.verification);
        if (method != DT_GRID_OVERVIEW_NEAREST &&
            (result.width > result.source_window.width ||
             result.height > result.source_window.height)) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "aggregate GRID view cannot upsample");
        }
        if (cancelled && cancelled()) {
            throw Exception(DT_E_CANCELLED,
                            "terrain operation was cancelled");
        }

        if ((request.flags & DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE) != 0) {
            source->prefetch_window(result.source_window.column,
                                    result.source_window.row,
                                    result.source_window.width,
                                    result.source_window.height);
            result.flags |= DT_GRID_VIEW_RESULT_PREFETCH_REQUESTED;
        }
        const bool verify =
            (request.flags & DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS) != 0;
        const double tile_progress_begin = verify ? 0.25 : 0.0;
        if (verify) {
            result.verification = source->verify_window(
                result.source_window.column, result.source_window.row,
                result.source_window.width, result.source_window.height,
                [&](double value) {
                    if (progress) progress(value * tile_progress_begin);
                },
                cancelled);
            result.flags |= DT_GRID_VIEW_RESULT_SOURCE_VERIFIED;
        }

        uint64_t scale = 1;
        if (method != DT_GRID_OVERVIEW_NEAREST) {
            const uint64_t ratio = std::min(
                result.source_window.width / result.width,
                result.source_window.height / result.height);
            while (scale <= ratio / 2U) scale *= 2U;
        }
        result.lod_scale = scale;
        const uint64_t source_columns_per_tile =
            static_cast<uint64_t>(tile_width) * scale;
        const uint64_t source_rows_per_tile =
            static_cast<uint64_t>(tile_height) * scale;
        const uint64_t first_tile_x =
            result.source_window.column / source_columns_per_tile;
        const uint64_t first_tile_y =
            result.source_window.row / source_rows_per_tile;
        const uint64_t last_tile_x =
            (result.source_window.column + result.source_window.width - 1) /
            source_columns_per_tile;
        const uint64_t last_tile_y =
            (result.source_window.row + result.source_window.height - 1) /
            source_rows_per_tile;
        const uint64_t tile_columns = last_tile_x - first_tile_x + 1;
        const uint64_t tile_rows = last_tile_y - first_tile_y + 1;
        if (tile_columns > kMaximumRequestTiles ||
            tile_rows > kMaximumRequestTiles / tile_columns) {
            throw Exception(DT_E_LIMIT_EXCEEDED,
                            "GRID view intersects too many cache tiles");
        }
        result.tile_count = tile_columns * tile_rows;

        struct ScheduledTile {
            uint64_t x = 0;
            uint64_t y = 0;
            long double priority = 0.0L;
        };
        std::vector<ScheduledTile> schedule;
        schedule.reserve(static_cast<size_t>(result.tile_count));
        const long double center_x =
            (static_cast<long double>(result.source_window.column) +
             static_cast<long double>(result.source_window.width - 1) / 2.0L) /
            static_cast<long double>(source_columns_per_tile);
        const long double center_y =
            (static_cast<long double>(result.source_window.row) +
             static_cast<long double>(result.source_window.height - 1) / 2.0L) /
            static_cast<long double>(source_rows_per_tile);
        for (uint64_t y = first_tile_y; y <= last_tile_y; ++y) {
            for (uint64_t x = first_tile_x; x <= last_tile_x; ++x) {
                const long double dx = static_cast<long double>(x) + 0.5L - center_x;
                const long double dy = static_cast<long double>(y) + 0.5L - center_y;
                schedule.push_back({x, y, dx * dx + dy * dy});
            }
        }
        std::sort(schedule.begin(), schedule.end(),
                  [](const ScheduledTile& a, const ScheduledTile& b) {
                      if (a.priority != b.priority)
                          return a.priority < b.priority;
                      if (a.y != b.y) return a.y < b.y;
                      return a.x < b.x;
                  });

        const uint32_t tile_flags =
            ((request.flags & DT_GRID_VIEW_REQUEST_STRICT_NODATA) != 0
                 ? static_cast<uint32_t>(DT_GRID_OVERVIEW_STRICT_NODATA)
                 : 0U) |
            ((request.flags & DT_GRID_VIEW_REQUEST_USE_PYRAMID) != 0
                 ? static_cast<uint32_t>(DT_GRID_OVERVIEW_USE_PYRAMID)
                 : 0U);
        const uint64_t generation = source->info().generation;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++request_count;
        }
        std::vector<Lease> leases;
        leases.reserve(schedule.size());
        std::vector<std::shared_ptr<Entry>> directory(
            static_cast<size_t>(result.tile_count));
        std::vector<Reuse> reuse_directory(
            static_cast<size_t>(result.tile_count), Reuse::Miss);
        try {
            for (const auto& tile : schedule) {
                Key key{generation, scale, tile.x, tile.y, method, tile_flags};
                Lease lease = acquire(key, tile.priority);
                if (lease.reuse != Reuse::Miss) ++result.reused_tile_count;
                if (lease.reuse == Reuse::Hit)
                    result.flags |= DT_GRID_VIEW_RESULT_CACHE_HIT;
                if (lease.reuse == Reuse::Coalesced)
                    result.flags |= DT_GRID_VIEW_RESULT_CACHE_COALESCED;
                const uint64_t local_x = tile.x - first_tile_x;
                const uint64_t local_y = tile.y - first_tile_y;
                const size_t directory_index = static_cast<size_t>(
                    local_y * tile_columns + local_x);
                directory[directory_index] = lease.entry;
                reuse_directory[directory_index] = lease.reuse;
                leases.push_back(std::move(lease));
            }

            constexpr double compose_progress_begin = 0.8;
            const double tile_progress_span =
                compose_progress_begin - tile_progress_begin;
            uint64_t completed_tiles = 0;
            bool used_pyramid = false;
            for (const auto& tile : schedule) {
                const uint64_t local_x = tile.x - first_tile_x;
                const uint64_t local_y = tile.y - first_tile_y;
                const size_t directory_index = static_cast<size_t>(
                    local_y * tile_columns + local_x);
                auto& entry = directory[directory_index];
                wait_ready(entry, cancelled);
                if (entry->loaded_from_disk)
                    result.flags |= DT_GRID_VIEW_RESULT_DISK_CACHE_HIT;
                if (entry->recovered_disk_payload &&
                    reuse_directory[directory_index] == Reuse::Disk &&
                    result.reused_tile_count != 0) {
                    --result.reused_tile_count;
                }
                used_pyramid = used_pyramid ||
                    (entry->overview.flags & DT_GRID_OVERVIEW_USED_PYRAMID) != 0;
                ++completed_tiles;
                if (progress) {
                    progress(tile_progress_begin + tile_progress_span *
                             static_cast<double>(completed_tiles) /
                                 static_cast<double>(result.tile_count));
                }
            }

            result.values.resize(static_cast<size_t>(result.width * result.height));
            uint64_t valid = 0;
            uint64_t nodata = 0;
            long double sum = 0.0L;
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            for (uint64_t output_y = 0; output_y < result.height; ++output_y) {
                if (cancelled && cancelled()) {
                    throw Exception(DT_E_CANCELLED,
                                    "terrain operation was cancelled");
                }
                const uint64_t source_y = result.source_window.row + std::min(
                    result.source_window.height - 1,
                    nearest_index(output_y, result.source_window.height,
                                  result.height));
                const uint64_t tile_y = source_y / source_rows_per_tile;
                for (uint64_t output_x = 0; output_x < result.width; ++output_x) {
                    const uint64_t source_x = result.source_window.column +
                        std::min(result.source_window.width - 1,
                                 nearest_index(output_x,
                                               result.source_window.width,
                                               result.width));
                    const uint64_t tile_x = source_x / source_columns_per_tile;
                    const auto& entry = directory[static_cast<size_t>(
                        (tile_y - first_tile_y) * tile_columns +
                        (tile_x - first_tile_x))];
                    const uint64_t source_local_x =
                        source_x - entry->source_column;
                    const uint64_t source_local_y =
                        source_y - entry->source_row;
                    const uint64_t sample_x = std::min(
                        entry->width - 1,
                        source_local_x * entry->width / entry->source_width);
                    const uint64_t sample_y = std::min(
                        entry->height - 1,
                        source_local_y * entry->height / entry->source_height);
                    const double value = entry->values[static_cast<size_t>(
                        sample_y * entry->width + sample_x)];
                    result.values[static_cast<size_t>(output_y * result.width +
                                                      output_x)] = value;
                    if (source->is_nodata(value)) {
                        ++nodata;
                    } else {
                        ++valid;
                        sum += static_cast<long double>(value);
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);
                    }
                }
                if (progress) {
                    progress(compose_progress_begin +
                        (1.0 - compose_progress_begin) *
                        static_cast<double>(output_y + 1) /
                        static_cast<double>(result.height));
                }
            }
            result.overview.struct_size = sizeof(result.overview);
            result.overview.flags = DT_GRID_OVERVIEW_USED_TILE_CACHE;
            if (used_pyramid)
                result.overview.flags |= DT_GRID_OVERVIEW_USED_PYRAMID;
            result.overview.valid_value_count = valid;
            result.overview.nodata_value_count = nodata;
            if (valid != 0) {
                result.overview.minimum_value = minimum;
                result.overview.maximum_value = maximum;
                result.overview.mean_value = static_cast<double>(
                    sum / static_cast<long double>(valid));
            } else {
                const double nan = std::numeric_limits<double>::quiet_NaN();
                result.overview.minimum_value = nan;
                result.overview.maximum_value = nan;
                result.overview.mean_value = nan;
            }
        } catch (...) {
            for (const auto& lease : leases) release(lease.entry);
            throw;
        }
        for (const auto& lease : leases) release(lease.entry);
        return result;
    }

    dt_grid_view_cache_statistics statistics() const {
        std::lock_guard<std::mutex> lock(mutex);
        dt_grid_view_cache_statistics result{};
        result.struct_size = sizeof(result);
        result.capacity_bytes = maximum_bytes;
        result.cached_bytes = cached_bytes;
        result.cached_tile_count = cached_tiles;
        result.in_flight_tile_count = in_flight_tiles;
        result.request_count = request_count;
        result.hit_tile_count = hit_tile_count;
        result.miss_tile_count = miss_tile_count;
        result.coalesced_tile_count = coalesced_tile_count;
        result.eviction_count = eviction_count;
        return result;
    }

    dt_grid_view_disk_cache_statistics disk_statistics() const {
        if (!disk_package) {
            throw Exception(DT_E_NOT_FOUND,
                            "GRID view cache has no DGTILE package");
        }
        return disk_package->statistics();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = entries.begin(); it != entries.end();) {
            auto& entry = *it->second;
            if (entry.demand == 0 && entry.state == Ready) {
                cached_bytes -= entry.values.size() * sizeof(double);
                --cached_tiles;
                ++eviction_count;
                it = entries.erase(it);
            } else if (entry.demand == 0 && entry.state == Failed) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }
};

GridViewCache::GridViewCache(std::shared_ptr<Grid> source,
                             const dt_grid_view_cache_options& options)
    : impl_(std::make_unique<Impl>(std::move(source), options)) {}

GridViewCache::GridViewCache(
    std::shared_ptr<Grid> source, const dt_grid_view_cache_options& options,
    const dt_grid_view_disk_cache_options& disk_options)
    : impl_(std::make_unique<Impl>(std::move(source), options, &disk_options)) {}

GridViewCache::~GridViewCache() = default;

CachedGridView GridViewCache::read_view(
    const dt_grid_view_request_options& options,
    const ProgressCallback& progress, const CancelCallback& cancelled) {
    return impl_->read_view(options, progress, cancelled);
}

dt_grid_view_cache_statistics GridViewCache::statistics() const {
    return impl_->statistics();
}

dt_grid_view_disk_cache_statistics GridViewCache::disk_statistics() const {
    return impl_->disk_statistics();
}

void GridViewCache::clear() { impl_->clear(); }

} // namespace dt
