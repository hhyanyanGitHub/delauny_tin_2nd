#include "dt_task_api.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void require_ok(dt_status status) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << "status=" << status << " error=" << error << '\n';
    std::abort();
}

dt_grid_handle create_grid(const double (&transform)[6]) {
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    options.width = 10;
    options.height = 8;
    for (int index = 0; index < 6; ++index)
        options.geo_transform[index] = transform[index];
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&options, &grid));
    std::vector<double> values(80);
    for (uint64_t row = 0; row < 8; ++row)
        for (uint64_t column = 0; column < 10; ++column)
            values[static_cast<size_t>(row * 10 + column)] =
                static_cast<double>(row * 10 + column);
    require_ok(dt_grid_write_window(grid, 0, 0, 10, 8, values.data(), 10));
    return grid;
}

dt_grid_handle create_identity_grid(uint64_t width, uint64_t height) {
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    options.width = width;
    options.height = height;
    options.geo_transform[1] = 1.0;
    options.geo_transform[5] = 1.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&options, &grid));
    std::vector<double> values(static_cast<size_t>(width * height));
    for (uint64_t row = 0; row < height; ++row)
        for (uint64_t column = 0; column < width; ++column)
            values[static_cast<size_t>(row * width + column)] =
                static_cast<double>(row * width + column);
    require_ok(dt_grid_write_window(grid, 0, 0, width, height, values.data(),
                                    width));
    return grid;
}

dt_grid_window query(dt_grid_handle grid, double xmin, double ymin,
                     double xmax, double ymax, uint32_t padding = 0) {
    dt_grid_view_options options{};
    options.struct_size = sizeof(options);
    options.world_bounds = {xmin, ymin, xmax, ymax};
    options.padding_nodes = padding;
    dt_grid_window window{};
    require_ok(dt_grid_get_view_window(grid, &options, &window));
    return window;
}

void test_identity_and_overview_composition() {
    const double transform[6]{100.0, 2.0, 0.0, 200.0, 0.0, -3.0};
    dt_grid_handle grid = create_grid(transform);
    const auto exact = query(grid, 104.2, 187.1, 111.8, 196.9);
    assert(exact.struct_size == sizeof(dt_grid_window));
    assert(exact.flags == 0);
    assert(exact.column == 2 && exact.row == 1);
    assert(exact.width == 5 && exact.height == 5);

    const auto padded = query(grid, 104.2, 187.1, 111.8, 196.9, 2);
    assert(padded.column == 0 && padded.row == 0);
    assert(padded.width == 9 && padded.height == 8);

    dt_grid_overview_options overview{};
    overview.struct_size = sizeof(overview);
    overview.source_column = exact.column;
    overview.source_row = exact.row;
    overview.source_width = exact.width;
    overview.source_height = exact.height;
    std::vector<double> output(6);
    require_ok(dt_grid_read_overview(grid, &overview, 3, 2, output.data(), 0,
                                     nullptr));
    assert(std::abs(output[0] - 17.0) < 1e-12);
    assert(std::abs(output[5] - 45.5) < 1e-12);

    const auto clipped = query(grid, 80.0, 185.0, 106.0, 230.0);
    assert((clipped.flags & DT_GRID_VIEW_WINDOW_CLIPPED) != 0);
    assert(clipped.column == 0 && clipped.row == 0);
    assert(clipped.width == 4 && clipped.height == 6);
    dt_grid_destroy(grid);
}

void test_rotated_sheared_and_validation() {
    const double transform[6]{500.0, 2.0, 0.75, 1000.0, -0.5, 1.5};
    dt_grid_handle grid = create_grid(transform);
    auto world = [&](double column, double row) {
        return dt_point3{transform[0] + column * transform[1] +
                             row * transform[2],
                         transform[3] + column * transform[4] +
                             row * transform[5],
                         0.0};
    };
    const dt_point3 corners[]{world(2.2, 1.4), world(5.8, 1.4),
                              world(5.8, 4.6), world(2.2, 4.6)};
    double xmin = corners[0].x, xmax = corners[0].x;
    double ymin = corners[0].y, ymax = corners[0].y;
    for (const auto& point : corners) {
        xmin = std::min(xmin, point.x);
        xmax = std::max(xmax, point.x);
        ymin = std::min(ymin, point.y);
        ymax = std::max(ymax, point.y);
    }
    const auto window = query(grid, xmin, ymin, xmax, ymax);
    assert(window.column <= 2 && window.row <= 1);
    assert(window.column + window.width - 1 >= 6);
    assert(window.row + window.height - 1 >= 5);

    dt_grid_view_options options{};
    options.struct_size = sizeof(options);
    options.world_bounds = {0.0, 0.0, 1.0, 1.0};
    dt_grid_window output{};
    assert(dt_grid_get_view_window(grid, &options, &output) == DT_E_NOT_FOUND);
    options.world_bounds = {1.0, 0.0, 0.0, 1.0};
    assert(dt_grid_get_view_window(grid, &options, &output) ==
           DT_E_INVALID_ARGUMENT);
    options.world_bounds = {0.0, 0.0, INFINITY, 1.0};
    assert(dt_grid_get_view_window(grid, &options, &output) ==
           DT_E_INVALID_ARGUMENT);
    options.world_bounds = {500.0, 990.0, 530.0, 1020.0};
    options.flags = 4;
    assert(dt_grid_get_view_window(grid, &options, &output) ==
           DT_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.padding_nodes = 1048577;
    assert(dt_grid_get_view_window(grid, &options, &output) ==
           DT_E_INVALID_ARGUMENT);
    assert(dt_grid_get_view_window(grid, &options, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(grid);
}

void test_unified_async_view_request() {
    const double transform[6]{100.0, 2.0, 0.0, 200.0, 0.0, -3.0};
    dt_grid_handle grid = create_grid(transform);
    dt_grid_view_request_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE;
    options.world_bounds = {104.2, 187.1, 111.8, 196.9};
    options.output_width = 3;
    options.output_height = 2;
    options.overview_method = DT_GRID_OVERVIEW_AVERAGE;
    options.tile_row_count = 1;

    dt_task_handle task = nullptr;
    require_ok(dt_grid_read_view_async(grid, &options, &task));
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed));
    assert(completed == 1);
    dt_task_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_task_get_info(task, &info));
    assert(info.state == DT_TASK_SUCCEEDED);
    assert(info.result_kind == DT_TASK_RESULT_GRID_VIEW);
    assert(info.progress == 1.0);

    dt_grid_view_result result{};
    result.struct_size = sizeof(result);
    require_ok(dt_task_get_grid_view_result(task, &result));
    assert((result.flags & DT_GRID_VIEW_RESULT_PREFETCH_REQUESTED) != 0);
    assert((result.flags & DT_GRID_VIEW_RESULT_SOURCE_VERIFIED) == 0);
    assert(result.source_window.column == 2 && result.source_window.row == 1);
    assert(result.source_window.width == 5 && result.source_window.height == 5);
    assert(result.width == 3 && result.height == 2 && result.row_stride == 3);
    assert(std::abs(result.values[0] - 17.0) < 1e-12);
    assert(std::abs(result.values[5] - 45.5) < 1e-12);
    assert((result.overview.flags &
            DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS) != 0);
    assert(result.verification.struct_size == sizeof(dt_grid_verify_result));
    dt_grid_overview_view wrong_result{};
    wrong_result.struct_size = sizeof(wrong_result);
    assert(dt_task_get_grid_overview_result(task, &wrong_result) ==
           DT_E_NOT_FOUND);
    dt_task_destroy(task);

    options.flags = 1u << 31;
    task = reinterpret_cast<dt_task_handle>(1);
    assert(dt_grid_read_view_async(grid, &options, &task) ==
           DT_E_INVALID_ARGUMENT);
    assert(task == nullptr);

    options.flags = 0;
    options.world_bounds = {0.0, 0.0, 1.0, 1.0};
    require_ok(dt_grid_read_view_async(grid, &options, &task));
    completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed));
    info = {};
    info.struct_size = sizeof(info);
    require_ok(dt_task_get_info(task, &info));
    assert(info.state == DT_TASK_FAILED);
    assert(info.result_status == DT_E_NOT_FOUND);
    result = {};
    result.struct_size = sizeof(result);
    assert(dt_task_get_grid_view_result(task, &result) == DT_E_NOT_FOUND);
    dt_task_destroy(task);
    dt_grid_destroy(grid);
}

void test_spatial_tile_cache_reuse_and_eviction() {
    dt_grid_handle grid = create_identity_grid(64, 32);
    dt_grid_view_cache_options cache_options{};
    cache_options.struct_size = sizeof(cache_options);
    cache_options.tile_width = 16;
    cache_options.tile_height = 16;
    cache_options.worker_count = 1;
    cache_options.maximum_bytes = 16ULL * 16ULL * sizeof(double);
    cache_options.maximum_tiles = 8;
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create(grid, &cache_options, &cache));

    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.world_bounds = {0.0, 0.0, 31.0, 15.0};
    request.output_width = 32;
    request.output_height = 16;
    request.overview_method = DT_GRID_OVERVIEW_NEAREST;

    auto read = [&](std::vector<double>& copied, dt_grid_view_result& view) {
        dt_task_handle task = nullptr;
        require_ok(dt_grid_read_view_cached_async(cache, &request, &task));
        int32_t completed = 0;
        require_ok(dt_task_wait(task, UINT32_MAX, &completed));
        assert(completed == 1);
        dt_task_info info{};
        info.struct_size = sizeof(info);
        require_ok(dt_task_get_info(task, &info));
        assert(info.state == DT_TASK_SUCCEEDED);
        view = {};
        view.struct_size = sizeof(view);
        require_ok(dt_task_get_grid_view_result(task, &view));
        copied.assign(view.values,
                      view.values + static_cast<size_t>(view.width * view.height));
        dt_task_destroy(task);
    };

    std::vector<double> first;
    dt_grid_view_result first_view{};
    read(first, first_view);
    assert((first_view.flags & DT_GRID_VIEW_RESULT_USED_TILE_CACHE) != 0);
    assert((first_view.overview.flags & DT_GRID_OVERVIEW_USED_TILE_CACHE) != 0);
    assert(first_view.lod_scale == 1);
    assert(first_view.tile_count >= 2);
    assert(first_view.reused_tile_count == 0);
    assert(first.front() == 0.0);

    dt_grid_view_cache_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_statistics(cache, &statistics));
    assert(statistics.request_count == 1);
    assert(statistics.miss_tile_count == first_view.tile_count);
    assert(statistics.eviction_count >= 1);
    assert(statistics.cached_tile_count <= 1);
    assert(statistics.cached_bytes <= statistics.capacity_bytes);

    std::vector<double> second;
    dt_grid_view_result second_view{};
    read(second, second_view);
    assert(first == second);
    assert(second_view.reused_tile_count >= 1);
    assert((second_view.flags & (DT_GRID_VIEW_RESULT_CACHE_HIT |
                                 DT_GRID_VIEW_RESULT_CACHE_COALESCED)) != 0);
    statistics = {};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_statistics(cache, &statistics));
    assert(statistics.request_count == 2);
    assert(statistics.hit_tile_count + statistics.coalesced_tile_count >= 1);

    require_ok(dt_grid_view_cache_clear(cache));
    statistics = {};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_statistics(cache, &statistics));
    assert(statistics.cached_tile_count == 0);
    assert(statistics.cached_bytes == 0);
    assert(dt_grid_view_cache_get_statistics(cache, nullptr) ==
           DT_E_INVALID_ARGUMENT);

    dt_grid_destroy(grid);
    std::vector<double> retained;
    dt_grid_view_result retained_view{};
    read(retained, retained_view);
    assert(retained == first);
    dt_grid_view_cache_destroy(cache);

    dt_grid_handle validation_grid = create_identity_grid(16, 16);
    cache_options.flags = 1;
    cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create(validation_grid, &cache_options, &cache) ==
           DT_E_INVALID_ARGUMENT);
    assert(cache == nullptr);
    dt_grid_destroy(validation_grid);
}

void test_spatial_tile_cache_inflight_coalescing() {
    dt_grid_handle grid = create_identity_grid(512, 512);
    dt_grid_view_cache_options cache_options{};
    cache_options.struct_size = sizeof(cache_options);
    cache_options.tile_width = 16;
    cache_options.tile_height = 16;
    cache_options.worker_count = 1;
    cache_options.maximum_bytes = 4ULL * 1024ULL * 1024ULL;
    cache_options.maximum_tiles = 2048;
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create(grid, &cache_options, &cache));

    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.world_bounds = {0.0, 0.0, 511.0, 511.0};
    request.output_width = 512;
    request.output_height = 512;
    request.overview_method = DT_GRID_OVERVIEW_NEAREST;
    dt_task_handle first = nullptr;
    dt_task_handle second = nullptr;
    require_ok(dt_grid_read_view_cached_async(cache, &request, &first));
    require_ok(dt_grid_read_view_cached_async(cache, &request, &second));
    int32_t completed = 0;
    require_ok(dt_task_wait(first, UINT32_MAX, &completed));
    assert(completed == 1);
    completed = 0;
    require_ok(dt_task_wait(second, UINT32_MAX, &completed));
    assert(completed == 1);
    dt_grid_view_result first_view{};
    first_view.struct_size = sizeof(first_view);
    dt_grid_view_result second_view{};
    second_view.struct_size = sizeof(second_view);
    require_ok(dt_task_get_grid_view_result(first, &first_view));
    require_ok(dt_task_get_grid_view_result(second, &second_view));
    assert(first_view.tile_count == 1024);
    assert(second_view.tile_count == first_view.tile_count);
    assert((first_view.flags & DT_GRID_VIEW_RESULT_CACHE_COALESCED) != 0 ||
           (second_view.flags & DT_GRID_VIEW_RESULT_CACHE_COALESCED) != 0);
    dt_grid_view_cache_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_statistics(cache, &statistics));
    assert(statistics.request_count == 2);
    assert(statistics.coalesced_tile_count != 0);
    assert(statistics.miss_tile_count <= first_view.tile_count);
    dt_task_destroy(first);
    dt_task_destroy(second);
    dt_grid_view_cache_destroy(cache);
    dt_grid_destroy(grid);
}

std::filesystem::path temporary_tile_package(const char* suffix) {
    return std::filesystem::temp_directory_path() /
        (std::string("dterrain-view-") + suffix + ".dgtile");
}

std::vector<double> read_cached_view(
    dt_grid_view_cache_handle cache,
    const dt_grid_view_request_options& request,
    dt_grid_view_result& output_view,
    dt_task_info* output_info = nullptr) {
    dt_task_handle task = nullptr;
    require_ok(dt_grid_read_view_cached_async(cache, &request, &task));
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed));
    assert(completed == 1);
    dt_task_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_task_get_info(task, &info));
    if (output_info) *output_info = info;
    std::vector<double> values;
    if (info.state == DT_TASK_SUCCEEDED) {
        output_view = {};
        output_view.struct_size = sizeof(output_view);
        require_ok(dt_task_get_grid_view_result(task, &output_view));
        values.assign(output_view.values,
                      output_view.values + static_cast<size_t>(
                          output_view.width * output_view.height));
    }
    dt_task_destroy(task);
    return values;
}

void test_persistent_tile_package_roundtrip_and_validation() {
    const auto path = temporary_tile_package("roundtrip");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    dt_grid_handle grid = create_identity_grid(32, 16);

    dt_grid_view_cache_options memory{};
    memory.struct_size = sizeof(memory);
    memory.tile_width = 16;
    memory.tile_height = 16;
    memory.worker_count = 1;
    memory.maximum_bytes = 1024ULL * 1024ULL;
    memory.maximum_tiles = 32;
    const std::string utf8_path = path.u8string();
    dt_grid_view_disk_cache_options disk{};
    disk.struct_size = sizeof(disk);
    dt_grid_view_cache_handle cache = nullptr;
    cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               grid, &memory, &disk, &cache) == DT_E_INVALID_ARGUMENT);
    assert(cache == nullptr);
    disk.utf8_file_name = utf8_path.c_str();
    disk.source_revision = 77;
    disk.maximum_file_bytes = 16ULL * 1024ULL * 1024ULL;

    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.world_bounds = {0.0, 0.0, 31.0, 15.0};
    request.output_width = 32;
    request.output_height = 16;
    request.overview_method = DT_GRID_OVERVIEW_NEAREST;

    cache = nullptr;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_grid_view_result first_view{};
    const auto first = read_cached_view(cache, request, first_view);
    assert((first_view.flags & DT_GRID_VIEW_RESULT_DISK_CACHE_HIT) == 0);
    dt_grid_view_disk_cache_statistics disk_statistics{};
    disk_statistics.struct_size = sizeof(disk_statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(
        cache, &disk_statistics));
    assert((disk_statistics.flags & DT_GRID_VIEW_DISK_CACHE_ACTIVE) != 0);
    assert(disk_statistics.indexed_tile_count == first_view.tile_count);
    assert(disk_statistics.written_tile_count == first_view.tile_count);
    assert(disk_statistics.file_bytes > 4096);
    const uint64_t fingerprint = disk_statistics.source_fingerprint;
    dt_grid_view_cache_handle competing =
        reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               grid, &memory, &disk, &competing) == DT_E_IO);
    assert(competing == nullptr);
    disk.flags = DT_GRID_VIEW_DISK_CACHE_READ_ONLY;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &competing));
    dt_grid_view_cache_destroy(competing);
    disk.flags = 0;
    dt_grid_view_cache_destroy(cache);

    cache = nullptr;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_grid_view_result second_view{};
    const auto second = read_cached_view(cache, request, second_view);
    assert(first == second);
    assert((second_view.flags & DT_GRID_VIEW_RESULT_DISK_CACHE_HIT) != 0);
    assert(second_view.reused_tile_count == second_view.tile_count);
    disk_statistics = {};
    disk_statistics.struct_size = sizeof(disk_statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(
        cache, &disk_statistics));
    assert(disk_statistics.disk_hit_tile_count == second_view.tile_count);
    assert(disk_statistics.source_fingerprint == fingerprint);
    dt_grid_view_cache_destroy(cache);

    const double changed = -1000.0;
    require_ok(dt_grid_write_window(grid, 0, 0, 1, 1, &changed, 1));
    cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               grid, &memory, &disk, &cache) == DT_E_STALE_QUERY);
    assert(cache == nullptr);
    disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_STALE;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    disk_statistics = {};
    disk_statistics.struct_size = sizeof(disk_statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(
        cache, &disk_statistics));
    assert(disk_statistics.indexed_tile_count == 0);
    assert(disk_statistics.file_bytes == 4096);
    dt_grid_view_cache_destroy(cache);

    dt_grid_view_cache_handle volatile_cache = nullptr;
    require_ok(dt_grid_view_cache_create(grid, &memory, &volatile_cache));
    disk_statistics = {};
    disk_statistics.struct_size = sizeof(disk_statistics);
    assert(dt_grid_view_cache_get_disk_statistics(
               volatile_cache, &disk_statistics) == DT_E_NOT_FOUND);
    dt_grid_view_cache_destroy(volatile_cache);
    dt_grid_destroy(grid);
    std::filesystem::remove(path, ignored);
}

void test_persistent_tile_package_corruption_and_capacity() {
    const auto path = temporary_tile_package("corrupt");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    dt_grid_handle grid = create_identity_grid(16, 16);
    dt_grid_view_cache_options memory{};
    memory.struct_size = sizeof(memory);
    memory.tile_width = 16;
    memory.tile_height = 16;
    memory.worker_count = 1;
    memory.maximum_bytes = 16ULL * 16ULL * sizeof(double);
    memory.maximum_tiles = 2;
    const std::string utf8_path = path.u8string();
    dt_grid_view_disk_cache_options disk{};
    disk.struct_size = sizeof(disk);
    disk.utf8_file_name = utf8_path.c_str();
    disk.maximum_file_bytes = 4096 + 128; // Header fits, payload does not.
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.world_bounds = {0.0, 0.0, 15.0, 15.0};
    request.output_width = 16;
    request.output_height = 16;
    request.overview_method = DT_GRID_OVERVIEW_NEAREST;
    dt_grid_view_result view{};
    read_cached_view(cache, request, view);
    dt_grid_view_disk_cache_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(cache, &statistics));
    assert(statistics.indexed_tile_count == 0);
    assert(statistics.skipped_write_count == 1);
    dt_grid_view_cache_destroy(cache);

    disk.maximum_file_bytes = 1024ULL * 1024ULL;
    disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    read_cached_view(cache, request, view);
    dt_grid_view_cache_destroy(cache);
    {
        std::fstream stream(path, std::ios::binary | std::ios::in |
                                  std::ios::out);
        assert(stream);
        stream.seekg(4096 + 128);
        char byte = 0;
        stream.read(&byte, 1);
        byte ^= static_cast<char>(0x7f);
        stream.seekp(4096 + 128);
        stream.write(&byte, 1);
    }
    disk.flags = 0;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_task_info failed_info{};
    read_cached_view(cache, request, view, &failed_info);
    assert(failed_info.state == DT_TASK_FAILED);
    assert(failed_info.result_status == DT_E_CORRUPTED_DATA);
    dt_grid_view_cache_destroy(cache);
    disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_task_info recovered_info{};
    const auto recovered = read_cached_view(
        cache, request, view, &recovered_info);
    assert(recovered_info.state == DT_TASK_SUCCEEDED);
    assert(recovered.size() == 16 * 16);
    assert((view.flags & DT_GRID_VIEW_RESULT_DISK_CACHE_HIT) == 0);
    statistics = {};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(cache, &statistics));
    assert(statistics.stored_record_count == 2);
    assert(statistics.indexed_tile_count == 1);
    assert(statistics.reclaimable_bytes != 0);
    dt_grid_view_cache_compact_result compact{};
    compact.struct_size = sizeof(compact);
    require_ok(dt_grid_view_cache_compact(cache, &compact));
    assert(compact.input_record_count == 2);
    assert(compact.output_record_count == 1);
    assert(compact.retained_tile_count == 1);
    assert(compact.dropped_duplicate_record_count == 1);
    assert(compact.reclaimed_bytes != 0);
    assert((compact.flags & DT_GRID_VIEW_COMPACT_SHRANK) != 0);
    dt_grid_view_cache_destroy(cache);
    {
        std::fstream stream(path, std::ios::binary | std::ios::in |
                                  std::ios::out);
        assert(stream);
        stream.seekp(200);
        const char damaged = static_cast<char>(0x5a);
        stream.write(&damaged, 1);
    }
    disk.flags = 0;
    cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               grid, &memory, &disk, &cache) == DT_E_CORRUPTED_DATA);
    assert(cache == nullptr);
    disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    statistics = {};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(cache, &statistics));
    assert(statistics.indexed_tile_count == 0);
    dt_grid_view_cache_destroy(cache);

    disk.flags = DT_GRID_VIEW_DISK_CACHE_READ_ONLY;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    statistics = {};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(cache, &statistics));
    assert((statistics.flags &
            DT_GRID_VIEW_DISK_CACHE_READ_ONLY_ACTIVE) != 0);
    compact = {};
    compact.struct_size = sizeof(compact);
    assert(dt_grid_view_cache_compact(cache, &compact) == DT_E_UNSUPPORTED);
    dt_grid_view_cache_destroy(cache);
    dt_grid_destroy(grid);
    std::filesystem::remove(path, ignored);
    cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               nullptr, &memory, &disk, &cache) == DT_E_NOT_INITIALIZED);
    assert(cache == nullptr);
}

void test_persistent_tile_package_compaction_drops_corrupt_latest_tile() {
    const auto path = temporary_tile_package("compact-corrupt");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    dt_grid_handle grid = create_identity_grid(32, 16);
    dt_grid_view_cache_options memory{};
    memory.struct_size = sizeof(memory);
    memory.tile_width = 16;
    memory.tile_height = 16;
    memory.worker_count = 1;
    memory.maximum_bytes = 1024ULL * 1024ULL;
    memory.maximum_tiles = 16;
    const std::string utf8_path = path.u8string();
    dt_grid_view_disk_cache_options disk{};
    disk.struct_size = sizeof(disk);
    disk.utf8_file_name = utf8_path.c_str();
    disk.maximum_file_bytes = 1024ULL * 1024ULL;
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_grid_view_request_options request{};
    request.struct_size = sizeof(request);
    request.world_bounds = {0.0, 0.0, 31.0, 15.0};
    request.output_width = 32;
    request.output_height = 16;
    request.overview_method = DT_GRID_OVERVIEW_NEAREST;
    dt_grid_view_result view{};
    read_cached_view(cache, request, view);
    dt_grid_view_cache_destroy(cache);

    {
        std::fstream stream(path, std::ios::binary | std::ios::in |
                                  std::ios::out);
        assert(stream);
        const std::streamoff second_payload = 4096 + 128 + 16 * 16 * 8 + 128;
        stream.seekg(second_payload);
        char byte = 0;
        stream.read(&byte, 1);
        byte ^= static_cast<char>(0x3d);
        stream.seekp(second_payload);
        stream.write(&byte, 1);
    }
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    dt_grid_view_cache_compact_result compact{};
    compact.struct_size = sizeof(compact);
    require_ok(dt_grid_view_cache_compact(cache, &compact));
    assert(compact.input_record_count == 2);
    assert(compact.output_record_count == 1);
    assert(compact.dropped_corrupt_tile_count == 1);
    assert((compact.flags & DT_GRID_VIEW_COMPACT_DROPPED_CORRUPTION) != 0);
    const auto values = read_cached_view(cache, request, view);
    assert(values.size() == 32 * 16);
    dt_grid_view_disk_cache_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    require_ok(dt_grid_view_cache_get_disk_statistics(cache, &statistics));
    assert(statistics.indexed_tile_count == 2);
    dt_grid_view_cache_destroy(cache);

    dt_grid_view_cache_handle volatile_cache = nullptr;
    require_ok(dt_grid_view_cache_create(grid, &memory, &volatile_cache));
    compact = {};
    compact.struct_size = sizeof(compact);
    assert(dt_grid_view_cache_compact(volatile_cache, &compact) ==
           DT_E_NOT_FOUND);
    assert(dt_grid_view_cache_compact(volatile_cache, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_view_cache_destroy(volatile_cache);
    dt_grid_destroy(grid);
    std::filesystem::remove(path, ignored);
}

#ifdef _WIN32
int hold_dgtile_writer(const char* path_text, const char* ready_text,
                       const char* release_text) {
    dt_grid_handle grid = create_identity_grid(16, 16);
    dt_grid_view_cache_options memory{};
    memory.struct_size = sizeof(memory);
    memory.tile_width = 16;
    memory.tile_height = 16;
    memory.maximum_bytes = 1024ULL * 1024ULL;
    memory.maximum_tiles = 4;
    dt_grid_view_disk_cache_options disk{};
    disk.struct_size = sizeof(disk);
    disk.utf8_file_name = path_text;
    disk.source_revision = 991;
    disk.maximum_file_bytes = 1024ULL * 1024ULL;
    dt_grid_view_cache_handle cache = nullptr;
    require_ok(dt_grid_view_cache_create_persistent(
        grid, &memory, &disk, &cache));
    std::ofstream(ready_text) << "ready";
    for (int attempt = 0; attempt < 1500; ++attempt) {
        if (std::filesystem::exists(release_text)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    dt_grid_view_cache_destroy(cache);
    dt_grid_destroy(grid);
    return 0;
}

void test_cross_process_single_writer() {
    const auto path = temporary_tile_package("process-lock");
    auto ready = path;
    ready += ".ready";
    auto release = path;
    release += ".release";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(ready, ignored);
    std::filesystem::remove(release, ignored);
    char executable[MAX_PATH]{};
    assert(GetModuleFileNameA(nullptr, executable, MAX_PATH) != 0);
    std::string command = std::string("\"") + executable +
        "\" --hold-dgtile \"" + path.string() + "\" \"" +
        ready.string() + "\" \"" + release.string() + "\"";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    assert(CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                          &process));
    for (int attempt = 0; attempt < 500 && !std::filesystem::exists(ready);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(std::filesystem::exists(ready));

    dt_grid_handle grid = create_identity_grid(16, 16);
    dt_grid_view_cache_options memory{};
    memory.struct_size = sizeof(memory);
    memory.tile_width = 16;
    memory.tile_height = 16;
    memory.maximum_bytes = 1024ULL * 1024ULL;
    memory.maximum_tiles = 4;
    const std::string utf8_path = path.u8string();
    dt_grid_view_disk_cache_options disk{};
    disk.struct_size = sizeof(disk);
    disk.utf8_file_name = utf8_path.c_str();
    disk.source_revision = 991;
    disk.maximum_file_bytes = 1024ULL * 1024ULL;
    dt_grid_view_cache_handle cache = reinterpret_cast<dt_grid_view_cache_handle>(1);
    assert(dt_grid_view_cache_create_persistent(
               grid, &memory, &disk, &cache) == DT_E_IO);
    assert(cache == nullptr);
    dt_grid_destroy(grid);
    std::ofstream(release) << "release";
    assert(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    assert(exit_code == 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(ready, ignored);
    std::filesystem::remove(release, ignored);
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc == 5 && std::string(argv[1]) == "--hold-dgtile")
        return hold_dgtile_writer(argv[2], argv[3], argv[4]);
#endif
    static_assert(sizeof(dt_grid_view_options) == 64);
    static_assert(sizeof(dt_grid_window) == 64);
    static_assert(sizeof(dt_grid_view_request_options) == 96);
    static_assert(sizeof(dt_grid_view_cache_options) == 64);
    static_assert(sizeof(dt_grid_view_cache_statistics) == 96);
    static_assert(sizeof(dt_grid_view_disk_cache_options) == 64);
    static_assert(sizeof(dt_grid_view_disk_cache_statistics) == 96);
    static_assert(sizeof(dt_grid_view_cache_compact_result) == 96);
    test_identity_and_overview_composition();
    test_rotated_sheared_and_validation();
    test_unified_async_view_request();
    test_spatial_tile_cache_reuse_and_eviction();
    test_spatial_tile_cache_inflight_coalescing();
    test_persistent_tile_package_roundtrip_and_validation();
    test_persistent_tile_package_corruption_and_capacity();
    test_persistent_tile_package_compaction_drops_corrupt_latest_tile();
#ifdef _WIN32
    test_cross_process_single_writer();
#endif
    std::cout << "All GRID view LOD tests passed.\n";
    return 0;
}
