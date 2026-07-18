#include "dt_task_api.h"
#include "dt_terrain_api.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[1024]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed with status " << status << ": "
              << error << '\n';
    std::abort();
}

bool close(double a, double b, double tolerance = 1e-11) {
    return std::abs(a - b) <=
           tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

dt_point3 world_point(const double gt[6], double column, double row) {
    return {gt[0] + column * gt[1] + row * gt[2],
            gt[3] + column * gt[4] + row * gt[5], 0.0};
}

dt_grid_handle make_grid(uint64_t width, uint64_t height,
                         const double gt[6], bool nodata = false) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = nodata ? DT_GRID_HAS_NODATA : 0;
    create.width = width;
    create.height = height;
    std::copy(gt, gt + 6, create.geo_transform);
    create.nodata_value = -9999.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create GRID");
    std::vector<double> values(static_cast<size_t>(width * height));
    for (uint64_t row = 0; row < height; ++row)
        for (uint64_t column = 0; column < width; ++column)
            values[static_cast<size_t>(row * width + column)] =
                static_cast<double>(row * 100 + column);
    require_ok(dt_grid_write_window(grid, 0, 0, width, height,
                                    values.data(), width),
               "write GRID");
    require_ok(dt_grid_set_crs_wkt(grid, "LOCAL"), "set CRS");
    return grid;
}

std::vector<double> read_grid(dt_grid_handle grid, dt_grid_info* info = nullptr) {
    dt_grid_info local{};
    local.struct_size = sizeof(local);
    require_ok(dt_grid_get_info(grid, &local), "get GRID info");
    std::vector<double> values(static_cast<size_t>(local.width * local.height));
    require_ok(dt_grid_read_window(grid, 0, 0, local.width, local.height,
                                   values.data(), local.width),
               "read GRID");
    if (info) *info = local;
    return values;
}

void test_mask_crop_invert_and_affine() {
    const double gt[6] = {100.0, 2.0, 0.5, 200.0, -0.25, 1.5};
    dt_grid_handle source = make_grid(6, 5, gt);
    const std::vector<dt_point3> polygon{
        world_point(gt, 1, 1), world_point(gt, 4, 1),
        world_point(gt, 4, 3), world_point(gt, 1, 3)};

    dt_grid_clip_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = 1;
    options.output_nodata_value = -12345.0;
    dt_grid_handle masked = nullptr;
    require_ok(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                    &options, &masked),
               "mask polygon");
    dt_grid_info info{};
    const auto masked_values = read_grid(masked, &info);
    assert(info.width == 6 && info.height == 5);
    assert(info.valid_value_count == 12);
    assert(info.generation == 2);
    for (uint64_t row = 0; row < 5; ++row) {
        for (uint64_t column = 0; column < 6; ++column) {
            const double value = masked_values[static_cast<size_t>(row * 6 + column)];
            const bool inside = column >= 1 && column <= 4 &&
                                row >= 1 && row <= 3;
            assert(inside ? close(value, row * 100.0 + column)
                          : value == -12345.0);
        }
    }

    options.flags = DT_GRID_CLIP_INVERT;
    dt_grid_handle inverted = nullptr;
    require_ok(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                    &options, &inverted),
               "invert polygon mask");
    read_grid(inverted, &info);
    assert(info.valid_value_count == 18);

    options.flags = DT_GRID_CLIP_CROP_TO_BOUNDS;
    options.worker_count = 4;
    options.tile_row_count = 1;
    dt_grid_handle cropped = nullptr;
    require_ok(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                    &options, &cropped),
               "crop polygon");
    const auto cropped_values = read_grid(cropped, &info);
    assert(info.width == 4 && info.height == 3);
    assert(info.valid_value_count == 12);
    const dt_point3 expected_origin = world_point(gt, 1, 1);
    assert(close(info.geo_transform[0], expected_origin.x));
    assert(close(info.geo_transform[3], expected_origin.y));
    for (int index = 1; index < 6; ++index) {
        if (index == 3) continue;
        assert(close(info.geo_transform[index], gt[index]));
    }
    for (uint64_t row = 0; row < 3; ++row)
        for (uint64_t column = 0; column < 4; ++column)
            assert(close(cropped_values[static_cast<size_t>(row * 4 + column)],
                         (row + 1) * 100.0 + column + 1));

    dt_grid_destroy(cropped);
    dt_grid_destroy(inverted);
    dt_grid_destroy(masked);
    dt_grid_destroy(source);
}

void test_async_validation_and_cancel() {
    const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    dt_grid_handle source = make_grid(8, 8, gt);
    const std::vector<dt_point3> polygon{{1, 1, 7}, {6, 1, 8},
                                         {6, 6, 9}, {1, 6, 10}};
    dt_grid_clip_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_CLIP_CROP_TO_BOUNDS;
    dt_task_handle task = nullptr;
    require_ok(dt_grid_clip_polygon_async(source, polygon.data(), polygon.size(),
                                          &options, &task),
               "start async clip");
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed), "wait async clip");
    dt_task_info task_info{};
    task_info.struct_size = sizeof(task_info);
    require_ok(dt_task_get_info(task, &task_info), "get async clip info");
    assert(task_info.state == DT_TASK_SUCCEEDED);
    assert(task_info.result_kind == DT_TASK_RESULT_GRID);
    dt_grid_handle result = nullptr;
    require_ok(dt_task_get_grid_result(task, &result), "get async clip result");
    dt_grid_info info{};
    read_grid(result, &info);
    assert(info.width == 6 && info.height == 6);
    dt_grid_destroy(result);
    dt_task_destroy(task);

    options.flags = DT_GRID_CLIP_CROP_TO_BOUNDS | DT_GRID_CLIP_INVERT;
    assert(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                &options, &result) == DT_E_INVALID_ARGUMENT);
    options.flags = 8;
    assert(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                &options, &result) == DT_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.worker_count = 65;
    assert(dt_grid_clip_polygon(source, polygon.data(), polygon.size(),
                                &options, &result) == DT_E_INVALID_ARGUMENT);
    options.worker_count = 0;
    std::vector<dt_point3> duplicate{{0, 0, 0}, {1, 0, 0},
                                     {1, 0, 3}, {0, 1, 0}};
    assert(dt_grid_clip_polygon(source, duplicate.data(), duplicate.size(),
                                &options, &result) == DT_E_INVALID_ARGUMENT);
    std::vector<dt_point3> outside{{20, 20, 0}, {21, 20, 0},
                                   {21, 21, 0}, {20, 21, 0}};
    options.flags = DT_GRID_CLIP_CROP_TO_BOUNDS;
    assert(dt_grid_clip_polygon(source, outside.data(), outside.size(),
                                &options, &result) == DT_E_NOT_FOUND);
    std::vector<dt_point3> subnode_triangle{{0.2, 0.2, 0}, {1.8, 0.2, 0},
                                             {0.2, 1.2, 0}};
    assert(dt_grid_clip_polygon(source, subnode_triangle.data(),
                                subnode_triangle.size(), &options,
                                &result) == DT_E_NOT_FOUND);
    std::vector<dt_point3> collinear{{0, 0, 0}, {1, 1, 0}, {2, 2, 0}};
    assert(dt_grid_clip_polygon(source, collinear.data(), collinear.size(),
                                &options, &result) == DT_E_INVALID_ARGUMENT);

    dt_grid_destroy(source);
    dt_grid_handle large = make_grid(8192, 1024, gt);
    const std::vector<dt_point3> large_polygon{{1, 1, 0}, {8190, 1, 0},
                                               {8190, 1022, 0}, {1, 1022, 0}};
    options.flags = 0;
    require_ok(dt_grid_clip_polygon_async(large, large_polygon.data(),
                                          large_polygon.size(), &options, &task),
               "start cancellable clip");
    require_ok(dt_task_request_cancel(task), "request clip cancellation");
    require_ok(dt_task_wait(task, UINT32_MAX, &completed), "wait cancelled clip");
    task_info = {};
    task_info.struct_size = sizeof(task_info);
    require_ok(dt_task_get_info(task, &task_info), "get cancelled clip info");
    assert(task_info.state == DT_TASK_CANCELLED);
    assert(task_info.result_status == DT_E_CANCELLED);
    dt_task_destroy(task);
    dt_grid_destroy(large);
}

} // namespace

int main() {
    static_assert(sizeof(dt_grid_clip_options) == 64);
    test_mask_crop_invert_and_affine();
    test_async_validation_and_cancel();
    std::cout << "All GRID polygon clip tests passed.\n";
    return 0;
}
