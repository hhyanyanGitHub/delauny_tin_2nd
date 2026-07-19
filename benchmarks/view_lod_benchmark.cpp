#include "dt_task_api.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed: " << status << " " << error << '\n';
    std::exit(2);
}

dt_grid_handle create_grid(uint64_t width, uint64_t height) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = width;
    create.height = height;
    create.geo_transform[1] = 1.0;
    create.geo_transform[5] = 1.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create GRID");
    std::vector<double> row(static_cast<size_t>(width));
    for (uint64_t y = 0; y < height; ++y) {
        for (uint64_t x = 0; x < width; ++x) {
            row[static_cast<size_t>(x)] =
                100.0 + 18.0 * std::sin(static_cast<double>(x) * 0.004) +
                11.0 * std::cos(static_cast<double>(y) * 0.006);
        }
        require_ok(dt_grid_write_window(grid, 0, y, width, 1,
                                        row.data(), width), "write GRID");
    }
    return grid;
}

double read_overview(dt_grid_handle grid, const dt_grid_window& window,
                     uint64_t output_size, std::vector<double>& output) {
    dt_grid_overview_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_OVERVIEW_AVERAGE;
    options.source_column = window.column;
    options.source_row = window.row;
    options.source_width = window.width;
    options.source_height = window.height;
    output.resize(static_cast<size_t>(output_size * output_size));
    const auto begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_read_overview(grid, &options, output_size, output_size,
                                     output.data(), 0, nullptr),
               "read overview");
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t width = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8192;
    const uint64_t height = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 4096;
    const uint64_t view_width = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1024;
    const uint64_t view_height = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 768;
    const uint64_t output_size = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 512;
    if (view_width > width || view_height > height ||
        view_width < output_size || view_height < output_size) return 1;

    dt_grid_handle grid = create_grid(width, height);
    dt_grid_window full{};
    full.column = full.row = 0;
    full.width = width;
    full.height = height;
    std::vector<double> full_output;
    std::vector<double> view_output;
    const double full_seconds = read_overview(grid, full, output_size, full_output);

    const uint64_t first_column = (width - view_width) / 2;
    const uint64_t first_row = (height - view_height) / 2;
    dt_grid_view_options view{};
    view.struct_size = sizeof(view);
    view.world_bounds = {
        static_cast<double>(first_column), static_cast<double>(first_row),
        static_cast<double>(first_column + view_width - 1),
        static_cast<double>(first_row + view_height - 1)};
    dt_grid_window window{};
    const auto query_begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_get_view_window(grid, &view, &window), "query view");
    const double query_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - query_begin).count();
    const double view_seconds = read_overview(grid, window, output_size,
                                              view_output);
    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.flags = DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE;
    request.world_bounds = view.world_bounds;
    request.output_width = output_size;
    request.output_height = output_size;
    request.overview_method = DT_GRID_OVERVIEW_AVERAGE;
    dt_task_handle task = nullptr;
    const auto async_submit_begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_read_view_async(grid, &request, &task),
               "submit unified view");
    const auto async_submit_end = std::chrono::steady_clock::now();
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed),
               "wait unified view");
    const auto async_wait_end = std::chrono::steady_clock::now();
    dt_grid_view_result async_view{};
    async_view.struct_size = sizeof(async_view);
    require_ok(dt_task_get_grid_view_result(task, &async_view),
               "get unified view");
    double checksum = 0.0;
    double async_checksum = 0.0;
    for (size_t index = 0; index < view_output.size(); index += 97)
        checksum += view_output[index];
    for (uint64_t index = 0;
         index < async_view.width * async_view.height; index += 97)
        async_checksum += async_view.values[index];

    dt_grid_view_cache_options cache_options{};
    cache_options.struct_size = sizeof(cache_options);
    cache_options.tile_width = 128;
    cache_options.tile_height = 128;
    cache_options.worker_count = 4;
    cache_options.maximum_bytes = 64ULL * 1024ULL * 1024ULL;
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create(grid, &cache_options, &cache),
               "create view cache");
    const auto cached_read = [&](const dt_grid_view_request_options& options,
                                 uint64_t& reused, double& sampled_checksum) {
        dt_task_handle cached_task = nullptr;
        const auto begin = std::chrono::steady_clock::now();
        require_ok(dt_grid_read_view_cached_async(cache, &options, &cached_task),
                   "submit cached view");
        int32_t cached_completed = 0;
        require_ok(dt_task_wait(cached_task, UINT32_MAX, &cached_completed),
                   "wait cached view");
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        dt_grid_view_result cached_view{};
        cached_view.struct_size = sizeof(cached_view);
        require_ok(dt_task_get_grid_view_result(cached_task, &cached_view),
                   "get cached view");
        reused = cached_view.reused_tile_count;
        sampled_checksum = 0.0;
        for (uint64_t index = 0;
             index < cached_view.width * cached_view.height; index += 97)
            sampled_checksum += cached_view.values[index];
        dt_task_destroy(cached_task);
        return seconds;
    };
    uint64_t cold_reused = 0;
    uint64_t warm_reused = 0;
    uint64_t pan_reused = 0;
    double cold_checksum = 0.0;
    double warm_checksum = 0.0;
    double pan_checksum = 0.0;
    const double cached_cold_seconds =
        cached_read(request, cold_reused, cold_checksum);
    const double cached_warm_seconds =
        cached_read(request, warm_reused, warm_checksum);
    dt_grid_view_request_options pan_request = request;
    pan_request.world_bounds.xmin += 128.0;
    pan_request.world_bounds.xmax += 128.0;
    const double cached_pan_seconds =
        cached_read(pan_request, pan_reused, pan_checksum);
    dt_grid_view_cache_statistics cache_statistics{};
    cache_statistics.struct_size = sizeof(cache_statistics);
    require_ok(dt_grid_view_cache_get_statistics(cache, &cache_statistics),
               "get view cache statistics");
    const double source_fraction =
        static_cast<double>(window.width * window.height) /
        static_cast<double>(width * height);
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << 'x' << height
              << " view_window=" << window.width << 'x' << window.height
              << " output=" << output_size << 'x' << output_size
              << " source_fraction=" << source_fraction
              << " query_seconds=" << query_seconds
              << " full_seconds=" << full_seconds
              << " view_seconds=" << view_seconds
              << " speedup=" << full_seconds / view_seconds
              << " unified_submit_us="
              << std::chrono::duration<double, std::micro>(
                     async_submit_end - async_submit_begin).count()
              << " unified_wait_seconds="
              << std::chrono::duration<double>(
                     async_wait_end - async_submit_end).count()
              << " unified_checksum_error="
              << std::abs(checksum - async_checksum)
              << " cached_cold_seconds=" << cached_cold_seconds
              << " cached_warm_seconds=" << cached_warm_seconds
              << " cached_pan_seconds=" << cached_pan_seconds
              << " cached_warm_speedup="
              << cached_cold_seconds / cached_warm_seconds
              << " cached_warm_reused=" << warm_reused
              << " cached_pan_reused=" << pan_reused
              << " cache_hits=" << cache_statistics.hit_tile_count
              << " cache_coalesced="
              << cache_statistics.coalesced_tile_count
              << " cached_repeat_error="
              << std::abs(cold_checksum - warm_checksum)
              << " checksum=" << checksum << '\n';
    dt_task_destroy(task);
    dt_grid_view_cache_destroy(cache);
    dt_grid_destroy(grid);
    return std::isfinite(checksum) &&
                   std::abs(checksum - async_checksum) < 1e-9 &&
                   std::abs(cold_checksum - warm_checksum) < 1e-9 &&
                   warm_reused != 0 && pan_reused != 0 &&
                   std::isfinite(pan_checksum)
               ? 0 : 3;
}
