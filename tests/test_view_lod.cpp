#include "dt_task_api.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

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

} // namespace

int main() {
    static_assert(sizeof(dt_grid_view_options) == 64);
    static_assert(sizeof(dt_grid_window) == 64);
    static_assert(sizeof(dt_grid_view_request_options) == 96);
    test_identity_and_overview_composition();
    test_rotated_sheared_and_validation();
    test_unified_async_view_request();
    std::cout << "All GRID view LOD tests passed.\n";
    return 0;
}
