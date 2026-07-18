#include "dt_terrain_api.h"

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
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << "x" << height
              << " overview=" << output_width << "x" << output_height
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial_seconds
              << " parallel_seconds=" << parallel_seconds
              << " speedup=" << serial_seconds / parallel_seconds
              << " checksum_error=" << checksum_error
              << " mean_error=" << mean_error << "\n";
    dt_grid_destroy(grid);
    return checksum_error == 0.0 && mean_error == 0.0 ? 0 : 3;
}
