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

dt_grid_handle create_grid(uint64_t width, uint64_t height) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = width;
    create.height = height;
    create.geo_transform[1] = 2.0;
    create.geo_transform[5] = -2.0;
    dt_grid_handle result = nullptr;
    require_ok(dt_grid_create(&create, &result), "create GRID");
    return result;
}

dt_grid_earthwork_result run(dt_grid_handle existing, dt_grid_handle design,
                             uint32_t workers, uint32_t tile_rows,
                             double& elapsed) {
    dt_grid_earthwork_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = workers;
    options.tile_row_count = tile_rows;
    dt_grid_earthwork_result result{};
    const auto begin = std::chrono::steady_clock::now();
    require_ok(dt_grid_compare_earthwork(existing, design, &options,
                                          &result, nullptr),
               "compare earthwork");
    elapsed = std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - begin)
                  .count();
    return result;
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
    auto existing = create_grid(width, height);
    auto design = create_grid(width, height);
    std::vector<double> existing_row(static_cast<size_t>(width));
    std::vector<double> design_row(static_cast<size_t>(width));
    for (uint64_t row = 0; row < height; ++row) {
        for (uint64_t column = 0; column < width; ++column) {
            const double x = static_cast<double>(column);
            const double y = static_cast<double>(row);
            existing_row[static_cast<size_t>(column)] =
                100.0 + 0.01 * x + 0.02 * y + 3.0 * std::sin(x * 0.01);
            design_row[static_cast<size_t>(column)] =
                existing_row[static_cast<size_t>(column)] +
                2.0 * std::sin(x * 0.004) * std::cos(y * 0.005);
        }
        require_ok(dt_grid_write_window(existing, 0, row, width, 1,
                                         existing_row.data(), width),
                   "write existing GRID");
        require_ok(dt_grid_write_window(design, 0, row, width, 1,
                                         design_row.data(), width),
                   "write design GRID");
    }
    double serial_seconds = 0.0;
    double parallel_seconds = 0.0;
    const auto serial = run(existing, design, 1, tile_rows, serial_seconds);
    const auto parallel = run(existing, design, workers, tile_rows,
                              parallel_seconds);
    const double volume_error =
        std::abs(serial.cut_volume - parallel.cut_volume) +
        std::abs(serial.fill_volume - parallel.fill_volume) +
        std::abs(serial.net_volume - parallel.net_volume);
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << "x" << height
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial_seconds
              << " parallel_seconds=" << parallel_seconds
              << " speedup=" << serial_seconds / parallel_seconds
              << " volume_error=" << volume_error << "\n";
    dt_grid_destroy(existing);
    dt_grid_destroy(design);
    return volume_error <= 1e-8 *
               std::max(1.0, std::abs(serial.net_volume))
        ? 0
        : 3;
}
