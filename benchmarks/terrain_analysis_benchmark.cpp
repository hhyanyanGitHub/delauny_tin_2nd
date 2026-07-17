#include "dt_terrain_api.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

struct RunResult {
    double seconds = 0.0;
    double checksum = 0.0;
};

bool check(dt_status status, const char* operation) {
    if (status == DT_OK) return true;
    char message[1024]{};
    dt_get_last_error(message, sizeof(message), nullptr);
    std::cerr << operation << " failed: " << message << '\n';
    return false;
}

RunResult run(dt_grid_handle source, uint32_t workers, uint32_t tile_rows,
              bool& ok) {
    dt_grid_terrain_options options{};
    options.struct_size = sizeof(options);
    options.kind = DT_GRID_TERRAIN_SLOPE_DEGREES;
    options.z_factor = 1.0;
    options.output_nodata_value = -9999.0;
    options.worker_count = workers;
    options.tile_row_count = tile_rows;
    dt_grid_handle result = nullptr;
    const auto begin = std::chrono::steady_clock::now();
    ok = check(dt_grid_derive_terrain(source, &options, &result),
               "terrain analysis");
    const auto end = std::chrono::steady_clock::now();
    RunResult measured{};
    measured.seconds = std::chrono::duration<double>(end - begin).count();
    if (ok) {
        dt_grid_info info{};
        ok = check(dt_grid_get_info(result, &info), "terrain result info");
        for (uint64_t i = 0; ok && i < 64; ++i) {
            const uint64_t column = (i * 104729U) % info.width;
            const uint64_t row = (i * 130363U) % info.height;
            double value = 0.0;
            ok = check(dt_grid_read_window(result, column, row, 1, 1,
                                           &value, 1),
                       "terrain checksum read");
            measured.checksum += value;
        }
    }
    dt_grid_destroy(result);
    return measured;
}

} // namespace

int main(int argc, char** argv) {
    uint64_t width = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2048;
    uint64_t height = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : width;
    uint32_t workers = argc > 3
                           ? static_cast<uint32_t>(
                                 std::strtoul(argv[3], nullptr, 10))
                           : 0;
    uint32_t tile_rows = argc > 4
                             ? static_cast<uint32_t>(
                                   std::strtoul(argv[4], nullptr, 10))
                             : 64;
    if (width < 2 || height < 2 || width > 1000000000ULL / height) {
        std::cerr << "invalid GRID dimensions\n";
        return 1;
    }

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = width;
    create.height = height;
    create.geo_transform[1] = 1.0;
    create.geo_transform[5] = 1.0;
    dt_grid_handle source = nullptr;
    if (!check(dt_grid_create(&create, &source), "GRID create")) return 2;
    std::vector<double> values(static_cast<size_t>(width * height));
    for (uint64_t row = 0; row < height; ++row) {
        for (uint64_t column = 0; column < width; ++column) {
            values[static_cast<size_t>(row * width + column)] =
                100.0 * std::sin(static_cast<double>(column) * 0.002) *
                    std::cos(static_cast<double>(row) * 0.003) +
                static_cast<double>(column + row) * 0.0001;
        }
    }
    if (!check(dt_grid_write_window(source, 0, 0, width, height,
                                    values.data(), width),
               "GRID write")) {
        dt_grid_destroy(source);
        return 3;
    }
    values.clear();
    values.shrink_to_fit();

    bool serial_ok = false;
    bool parallel_ok = false;
    const RunResult serial = run(source, 1, tile_rows, serial_ok);
    const RunResult parallel = run(source, workers, tile_rows, parallel_ok);
    dt_grid_destroy(source);
    const bool checksum_ok =
        std::abs(serial.checksum - parallel.checksum) <= 1e-10;
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << 'x' << height
              << " tile_rows=" << tile_rows
              << " requested_workers=" << workers
              << " serial_seconds=" << serial.seconds
              << " parallel_seconds=" << parallel.seconds
              << " speedup="
              << (parallel.seconds > 0.0
                      ? serial.seconds / parallel.seconds
                      : 0.0)
              << " checksum_match=" << (checksum_ok ? 1 : 0) << '\n';
    return serial_ok && parallel_ok && checksum_ok ? 0 : 4;
}
