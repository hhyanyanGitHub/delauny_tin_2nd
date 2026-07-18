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
    dt_grid_handle result = nullptr;
    require_ok(dt_grid_create(&create, &result), "create GRID");
    std::vector<double> row(static_cast<size_t>(width));
    for (uint64_t y = 0; y < height; ++y) {
        for (uint64_t x = 0; x < width; ++x)
            row[static_cast<size_t>(x)] =
                100.0 + 0.01 * static_cast<double>(x) +
                0.02 * static_cast<double>(y);
        require_ok(dt_grid_write_window(result, 0, y, width, 1,
                                         row.data(), width), "write GRID");
    }
    return result;
}

dt_grid_handle run(dt_grid_handle source, const std::vector<dt_point3>& polygon,
                   uint32_t workers, uint32_t tile_rows, double& seconds) {
    dt_grid_clip_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = workers;
    options.tile_row_count = tile_rows;
    dt_grid_handle result = nullptr;
    const auto begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                    &options, &result), "clip GRID");
    seconds = std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - begin).count();
    return result;
}

double checksum(dt_grid_handle grid) {
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_grid_get_info(grid, &info), "get GRID info");
    std::vector<double> row(static_cast<size_t>(info.width));
    double sum = 0.0;
    for (uint64_t y = 0; y < info.height; y += 127) {
        require_ok(dt_grid_read_window(grid, 0, y, info.width, 1,
                                       row.data(), info.width), "read GRID");
        for (uint64_t x = 0; x < info.width; x += 61)
            if (std::isfinite(row[static_cast<size_t>(x)]))
                sum += row[static_cast<size_t>(x)];
    }
    return sum;
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t width = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4096;
    const uint64_t height = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : width;
    const uint32_t workers = argc > 3
        ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 0;
    const uint32_t tile_rows = argc > 4
        ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 64;
    if (width < 8 || height < 8) return 1;
    dt_grid_handle source = create_grid(width, height);
    const double w = static_cast<double>(width - 1);
    const double h = static_cast<double>(height - 1);
    const std::vector<dt_point3> polygon{
        {0.08 * w, 0.22 * h, 0}, {0.31 * w, 0.05 * h, 0},
        {0.74 * w, 0.09 * h, 0}, {0.94 * w, 0.36 * h, 0},
        {0.86 * w, 0.82 * h, 0}, {0.57 * w, 0.96 * h, 0},
        {0.19 * w, 0.88 * h, 0}, {0.03 * w, 0.51 * h, 0}};
    double serial_seconds = 0.0;
    double parallel_seconds = 0.0;
    auto serial = run(source, polygon, 1, tile_rows, serial_seconds);
    const double serial_checksum = checksum(serial);
    dt_grid_destroy(serial);
    auto parallel = run(source, polygon, workers, tile_rows, parallel_seconds);
    const double parallel_checksum = checksum(parallel);
    const double error = std::abs(serial_checksum - parallel_checksum);
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << "x" << height
              << " vertices=" << polygon.size()
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial_seconds
              << " parallel_seconds=" << parallel_seconds
              << " speedup=" << serial_seconds / parallel_seconds
              << " checksum_error=" << error << "\n";
    dt_grid_destroy(parallel);
    dt_grid_destroy(source);
    return error == 0.0 ? 0 : 3;
}
