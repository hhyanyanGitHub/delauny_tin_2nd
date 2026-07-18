#include "dt_terrain_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const uint64_t width = argc > 1 ? std::stoull(argv[1]) : 8192;
    const uint64_t height = argc > 2 ? std::stoull(argv[2]) : 4096;
    const std::filesystem::path file = "grid_binary_benchmark.dgridb";
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_HAS_NODATA;
    options.width = width;
    options.height = height;
    options.geo_transform[1] = 1.0;
    options.geo_transform[5] = -1.0;
    options.nodata_value = -9999.0;
    dt_grid_handle grid = nullptr;
    if (dt_grid_create(&options, &grid) != DT_OK) return 1;
    std::vector<double> row(width);
    for (uint64_t y = 0; y < height; ++y) {
        for (uint64_t x = 0; x < width; ++x)
            row[x] = 200.0 + 20.0 * std::sin(x * 0.002) +
                     10.0 * std::cos(y * 0.003);
        if (dt_grid_write_window(grid, 0, y, width, 1, row.data(), 0) != DT_OK)
            return 2;
    }
    const auto save_begin = std::chrono::steady_clock::now();
    if (dt_grid_save_binary(grid, file.string().c_str()) != DT_OK) return 3;
    const auto save_end = std::chrono::steady_clock::now();
    dt_grid_destroy(grid);

    const auto open_begin = std::chrono::steady_clock::now();
    if (dt_grid_load_binary(file.string().c_str(), &grid) != DT_OK) return 4;
    const auto open_end = std::chrono::steady_clock::now();
    std::vector<double> preview(512 * 512);
    dt_grid_overview_options overview{};
    overview.struct_size = sizeof(overview);
    overview.method = DT_GRID_OVERVIEW_AVERAGE;
    const auto preview_begin = std::chrono::steady_clock::now();
    if (dt_grid_read_overview(grid, &overview, 512, 512, preview.data(), 0,
                              nullptr) != DT_OK)
        return 5;
    const auto preview_end = std::chrono::steady_clock::now();
    const uint64_t local_width = std::min<uint64_t>(1024, width);
    const uint64_t local_height = std::min<uint64_t>(768, height);
    std::vector<double> local(local_width * local_height);
    const auto local_begin = std::chrono::steady_clock::now();
    if (dt_grid_read_window(grid, (width - local_width) / 2,
                            (height - local_height) / 2, local_width,
                            local_height, local.data(), 0) != DT_OK)
        return 6;
    const auto local_end = std::chrono::steady_clock::now();

    const auto seconds = [](auto begin, auto end) {
        return std::chrono::duration<double>(end - begin).count();
    };
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << 'x' << height
              << " file_mib=" << std::filesystem::file_size(file) /
                     (1024.0 * 1024.0)
              << " save_seconds=" << seconds(save_begin, save_end)
              << " open_seconds=" << seconds(open_begin, open_end)
              << " persistent_overview_seconds="
              << seconds(preview_begin, preview_end)
              << " local_read_seconds=" << seconds(local_begin, local_end)
              << " checksum=" << preview.front() + preview.back() +
                                      local.front() + local.back()
              << '\n';
    dt_grid_destroy(grid);
    std::filesystem::remove(file);
    return 0;
}
