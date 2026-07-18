#include "dt_api.h"
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

dt_grid_handle create_grid(uint64_t width, uint64_t height,
                           double origin_x, double origin_y) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = width;
    create.height = height;
    create.geo_transform[0] = origin_x;
    create.geo_transform[1] = 1.0;
    create.geo_transform[3] = origin_y;
    create.geo_transform[5] = 1.0;
    dt_grid_handle result = nullptr;
    require_ok(dt_grid_create(&create, &result), "create GRID");
    return result;
}

dt_grid_handle run(dt_grid_handle source, dt_grid_handle reference,
                   uint32_t workers, uint32_t tile_rows, double& elapsed) {
    dt_grid_resample_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_RESAMPLE_BILINEAR;
    options.worker_count = workers;
    options.tile_row_count = tile_rows;
    dt_grid_handle result = nullptr;
    const auto begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_resample_like(source, reference, &options, &result),
               "resample GRID");
    elapsed = std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - begin)
                  .count();
    return result;
}

double checksum(dt_grid_handle grid) {
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_grid_get_info(grid, &info), "get output info");
    std::vector<double> row(static_cast<size_t>(info.width));
    double sum = 0.0;
    for (uint64_t y = 0; y < info.height; y += 127) {
        require_ok(dt_grid_read_window(grid, 0, y, info.width, 1,
                                       row.data(), info.width),
                   "read output row");
        for (uint64_t x = 0; x < info.width; x += 61)
            sum += row[static_cast<size_t>(x)];
    }
    return sum;
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t width = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4096;
    const uint64_t height = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : width;
    const uint32_t workers = argc > 3
        ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10))
        : 0;
    const uint32_t tile_rows = argc > 4
        ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10))
        : 64;
    if (width < 2 || height < 2) return 1;
    auto source = create_grid(width + 2, height + 2, 0.0, 0.0);
    auto reference = create_grid(width, height, 0.25, 0.25);
    std::vector<double> row(static_cast<size_t>(width + 2));
    for (uint64_t y = 0; y < height + 2; ++y) {
        for (uint64_t x = 0; x < width + 2; ++x) {
            row[static_cast<size_t>(x)] =
                100.0 + 0.01 * static_cast<double>(x) +
                0.02 * static_cast<double>(y) +
                3.0 * std::sin(static_cast<double>(x) * 0.01) *
                    std::cos(static_cast<double>(y) * 0.006);
        }
        require_ok(dt_grid_write_window(source, 0, y, width + 2, 1,
                                         row.data(), width + 2),
                   "write source GRID");
    }
    double serial_seconds = 0.0;
    double parallel_seconds = 0.0;
    auto serial = run(source, reference, 1, tile_rows, serial_seconds);
    const double serial_checksum = checksum(serial);
    dt_grid_destroy(serial);
    auto parallel = run(source, reference, workers, tile_rows,
                        parallel_seconds);
    const double parallel_checksum = checksum(parallel);
    const double checksum_error = std::abs(serial_checksum - parallel_checksum);
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << "x" << height
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial_seconds
              << " parallel_seconds=" << parallel_seconds
              << " speedup=" << serial_seconds / parallel_seconds
              << " checksum_error=" << checksum_error << "\n";
    dt_grid_destroy(parallel);
    dt_grid_destroy(reference);
    dt_grid_destroy(source);
    return checksum_error == 0.0 ? 0 : 3;
}
