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
    uint64_t previous_width = width;
    uint64_t previous_height = height;
    uint64_t pyramid_nodes = 0;
    uint64_t peak_scratch_doubles = 0;
    size_t level_index = 0;
    while (previous_width > 512 || previous_height > 512) {
        const uint64_t level_width = (previous_width + 1) / 2;
        const uint64_t level_height = (previous_height + 1) / 2;
        pyramid_nodes += level_width * level_height;
        const uint64_t scratch = level_index == 0
            ? level_width : previous_width * 2 + level_width;
        peak_scratch_doubles = std::max(peak_scratch_doubles, scratch);
        previous_width = level_width;
        previous_height = level_height;
        ++level_index;
    }
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
    const uint64_t pyramid_source_width = std::min<uint64_t>(4096, width);
    const uint64_t pyramid_source_height = std::min<uint64_t>(2048, height);
    const uint64_t pyramid_width = std::min<uint64_t>(512,
                                                       pyramid_source_width);
    const uint64_t pyramid_height = std::min<uint64_t>(256,
                                                        pyramid_source_height);
    std::vector<double> pyramid(pyramid_width * pyramid_height);
    overview.flags = 0;
    overview.source_column = (width - pyramid_source_width) / 2;
    overview.source_row = (height - pyramid_source_height) / 2;
    overview.source_width = pyramid_source_width;
    overview.source_height = pyramid_source_height;
    std::vector<double> exact_local(pyramid_width * pyramid_height);
    const auto exact_local_begin = std::chrono::steady_clock::now();
    if (dt_grid_read_overview(grid, &overview, pyramid_width, pyramid_height,
                              exact_local.data(), 0, nullptr) != DT_OK)
        return 6;
    const auto exact_local_end = std::chrono::steady_clock::now();
    overview.flags = DT_GRID_OVERVIEW_USE_PYRAMID;
    dt_grid_overview_result pyramid_result{};
    pyramid_result.struct_size = sizeof(pyramid_result);
    const auto pyramid_begin = std::chrono::steady_clock::now();
    if (dt_grid_read_overview(grid, &overview, pyramid_width, pyramid_height,
                              pyramid.data(), 0, &pyramid_result) != DT_OK ||
        (pyramid_result.flags & DT_GRID_OVERVIEW_USED_PYRAMID) == 0)
        return 7;
    const auto pyramid_end = std::chrono::steady_clock::now();
    const uint64_t local_width = std::min<uint64_t>(1024, width);
    const uint64_t local_height = std::min<uint64_t>(768, height);
    std::vector<double> local(local_width * local_height);
    dt_grid_prefetch_window(grid, (width - local_width) / 2,
                            (height - local_height) / 2, local_width,
                            local_height);
    const auto local_begin = std::chrono::steady_clock::now();
    if (dt_grid_read_window(grid, (width - local_width) / 2,
                            (height - local_height) / 2, local_width,
                            local_height, local.data(), 0) != DT_OK)
        return 8;
    const auto local_end = std::chrono::steady_clock::now();
    dt_grid_verify_result window_verify{};
    window_verify.struct_size = sizeof(window_verify);
    const auto window_verify_begin = std::chrono::steady_clock::now();
    if (dt_grid_verify_window(grid, (width - local_width) / 2,
                              (height - local_height) / 2, local_width,
                              local_height, &window_verify) != DT_OK)
        return 9;
    const auto window_verify_end = std::chrono::steady_clock::now();
    dt_grid_verify_result cached_verify{};
    cached_verify.struct_size = sizeof(cached_verify);
    const auto cached_verify_begin = std::chrono::steady_clock::now();
    if (dt_grid_verify_window(grid, (width - local_width) / 2,
                              (height - local_height) / 2, local_width,
                              local_height, &cached_verify) != DT_OK)
        return 10;
    const auto cached_verify_end = std::chrono::steady_clock::now();
    const auto verify_begin = std::chrono::steady_clock::now();
    if (dt_grid_verify_binary_file(file.string().c_str()) != DT_OK) return 11;
    const auto verify_end = std::chrono::steady_clock::now();

    const auto seconds = [](auto begin, auto end) {
        return std::chrono::duration<double>(end - begin).count();
    };
    std::cout << std::fixed << std::setprecision(6)
              << "grid=" << width << 'x' << height
              << " file_mib=" << std::filesystem::file_size(file) /
                     (1024.0 * 1024.0)
              << " legacy_pyramid_scratch_mib="
              << static_cast<double>(pyramid_nodes * sizeof(double)) /
                     (1024.0 * 1024.0)
              << " streaming_row_scratch_mib="
              << static_cast<double>(peak_scratch_doubles * sizeof(double)) /
                     (1024.0 * 1024.0)
              << " save_seconds=" << seconds(save_begin, save_end)
              << " open_seconds=" << seconds(open_begin, open_end)
              << " persistent_overview_seconds="
              << seconds(preview_begin, preview_end)
              << " exact_local_overview_seconds="
              << seconds(exact_local_begin, exact_local_end)
              << " pyramid_overview_seconds="
              << seconds(pyramid_begin, pyramid_end)
              << " local_read_seconds=" << seconds(local_begin, local_end)
              << " window_verify_seconds="
              << seconds(window_verify_begin, window_verify_end)
              << " cached_verify_seconds="
              << seconds(cached_verify_begin, cached_verify_end)
              << " window_blocks=" << window_verify.block_count
              << " cached_blocks=" << cached_verify.cached_block_count
              << " verify_seconds=" << seconds(verify_begin, verify_end)
              << " checksum=" << preview.front() + preview.back() +
                                      pyramid.front() + pyramid.back() +
                                      local.front() + local.back()
              << '\n';
    dt_grid_destroy(grid);
    std::filesystem::remove(file);
    return 0;
}
