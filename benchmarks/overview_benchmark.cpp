#include "dt_terrain_api.h"
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
    std::cerr << operation << " failed: " << status << " " << error << "\n";
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
                120.0 + 15.0 * std::sin(static_cast<double>(x) * 0.003) +
                9.0 * std::cos(static_cast<double>(y) * 0.004);
        }
        require_ok(dt_grid_write_window(grid, 0, y, width, 1,
                                         row.data(), width), "write GRID");
    }
    return grid;
}

double run(dt_grid_handle grid, uint64_t output_width, uint64_t output_height,
           uint32_t workers, uint32_t tile_rows, std::vector<double>& output,
           dt_grid_overview_result& result) {
    dt_grid_overview_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_OVERVIEW_AVERAGE;
    options.worker_count = workers;
    options.tile_row_count = tile_rows;
    output.resize(static_cast<size_t>(output_width * output_height));
    const auto begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_read_overview(grid, &options, output_width,
                                     output_height, output.data(), 0, &result),
               "read overview");
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - begin).count();
}

double checksum(const std::vector<double>& values) {
    double result = 0.0;
    for (size_t index = 0; index < values.size(); index += 97)
        result += values[index];
    return result;
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t width = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4096;
    const uint64_t height = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : width;
    const uint64_t output_width = argc > 3
        ? std::strtoull(argv[3], nullptr, 10) : 512;
    const uint64_t output_height = argc > 4
        ? std::strtoull(argv[4], nullptr, 10) : 512;
    const uint32_t workers = argc > 5
        ? static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 10)) : 0;
    const uint32_t tile_rows = argc > 6
        ? static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 10)) : 16;
    if (width < output_width || height < output_height ||
        output_width == 0 || output_height == 0) return 1;
    dt_grid_handle grid = create_grid(width, height);
    std::vector<double> serial;
    std::vector<double> parallel;
    dt_grid_overview_result serial_result{};
    dt_grid_overview_result parallel_result{};
    const double serial_seconds = run(grid, output_width, output_height, 1,
                                      tile_rows, serial, serial_result);
    const double parallel_seconds = run(grid, output_width, output_height,
                                        workers, tile_rows, parallel,
                                        parallel_result);
    const double checksum_error = std::abs(checksum(serial) - checksum(parallel));
    const double mean_error =
        std::abs(serial_result.mean_value - parallel_result.mean_value);

    dt_grid_overview_options async_options{};
    async_options.struct_size = sizeof(async_options);
    async_options.method = DT_GRID_OVERVIEW_AVERAGE;
    async_options.worker_count = workers;
    async_options.tile_row_count = tile_rows;
    dt_task_handle async_task = nullptr;
    const auto submit_begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_read_overview_async(
                   grid, &async_options, output_width, output_height,
                   &async_task),
               "submit async overview");
    const auto submit_end = std::chrono::steady_clock::now();
    int32_t completed = 0;
    require_ok(dt_task_wait(async_task, UINT32_MAX, &completed),
               "wait async overview");
    dt_grid_overview_view async_view{};
    async_view.struct_size = sizeof(async_view);
    require_ok(dt_task_get_grid_overview_result(async_task, &async_view),
               "get async overview");
    std::vector<double> async_values(
        async_view.values,
        async_view.values + async_view.width * async_view.height);
    const double async_checksum_error =
        std::abs(checksum(parallel) - checksum(async_values));
    const double async_submit_microseconds =
        std::chrono::duration<double, std::micro>(submit_end - submit_begin)
            .count();
    const double async_wait_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      submit_end)
            .count();
    dt_task_destroy(async_task);

    dt_task_handle cancel_task = nullptr;
    require_ok(dt_grid_read_overview_async(grid, &async_options, 1, 1,
                                            &cancel_task),
               "submit cancellable overview");
    const auto cancel_begin = std::chrono::steady_clock::now();
    require_ok(dt_task_request_cancel(cancel_task), "request cancellation");
    require_ok(dt_task_wait(cancel_task, UINT32_MAX, &completed),
               "wait cancellation");
    dt_task_info cancel_info{};
    cancel_info.struct_size = sizeof(cancel_info);
    require_ok(dt_task_get_info(cancel_task, &cancel_info),
               "get cancellation info");
    const double cancel_milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cancel_begin).count();
    dt_task_destroy(cancel_task);
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << "x" << height
              << " overview=" << output_width << "x" << output_height
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial_seconds
              << " parallel_seconds=" << parallel_seconds
              << " speedup=" << serial_seconds / parallel_seconds
              << " checksum_error=" << checksum_error
              << " mean_error=" << mean_error
              << " async_submit_us=" << async_submit_microseconds
              << " async_wait_seconds=" << async_wait_seconds
              << " async_checksum_error=" << async_checksum_error
              << " cancel_ms=" << cancel_milliseconds
              << " cancel_state=" << cancel_info.state << "\n";
    dt_grid_destroy(grid);
    return checksum_error == 0.0 && mean_error == 0.0 &&
                   async_checksum_error == 0.0
        ? 0
        : 3;
}
