#include "dt_task_api.h"
#include "dt_terrain_api.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
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

bool close(double a, double b, double tolerance = 1e-10) {
    return std::abs(a - b) <=
           tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

dt_grid_handle make_grid(uint64_t width, uint64_t height,
                         const double transform[6],
                         const std::vector<double>& values = {},
                         bool nodata = false,
                         double nodata_value = -9999.0,
                         const char* crs = "LOCAL") {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = nodata ? DT_GRID_HAS_NODATA : 0;
    create.width = width;
    create.height = height;
    std::copy(transform, transform + 6, create.geo_transform);
    create.nodata_value = nodata_value;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create GRID");
    if (!values.empty()) {
        assert(values.size() == width * height);
        require_ok(dt_grid_write_window(grid, 0, 0, width, height,
                                        values.data(), width),
                   "write GRID");
    }
    require_ok(dt_grid_set_crs_wkt(grid, crs), "set GRID CRS");
    return grid;
}

std::vector<double> read_grid(dt_grid_handle grid) {
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_grid_get_info(grid, &info), "get GRID info");
    std::vector<double> values(
        static_cast<size_t>(info.width * info.height));
    require_ok(dt_grid_read_window(grid, 0, 0, info.width, info.height,
                                   values.data(), info.width),
               "read GRID");
    return values;
}

double world_x(const double gt[6], double column, double row) {
    return gt[0] + column * gt[1] + row * gt[2];
}

double world_y(const double gt[6], double column, double row) {
    return gt[3] + column * gt[4] + row * gt[5];
}

void test_rotated_bilinear_and_parallel() {
    const double source_gt[6] = {100.0, 2.0, 0.5, 200.0, -0.25, 1.5};
    std::vector<double> values;
    for (uint64_t row = 0; row < 4; ++row) {
        for (uint64_t column = 0; column < 4; ++column) {
            const double x = world_x(source_gt, column, row);
            const double y = world_y(source_gt, column, row);
            values.push_back(2.0 * x - 3.0 * y + 5.0);
        }
    }
    dt_grid_handle source = make_grid(4, 4, source_gt, values);

    const double origin_column = 0.25;
    const double origin_row = 0.5;
    const double reference_gt[6] = {
        world_x(source_gt, origin_column, origin_row),
        source_gt[1] * 0.5, source_gt[2] * 0.5,
        world_y(source_gt, origin_column, origin_row),
        source_gt[4] * 0.5, source_gt[5] * 0.5};
    dt_grid_handle reference = make_grid(6, 5, reference_gt);

    dt_grid_resample_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_RESAMPLE_BILINEAR;
    options.worker_count = 1;
    dt_grid_handle serial = nullptr;
    require_ok(dt_grid_resample_like(source, reference, &options, &serial),
               "serial rotated resample");
    const auto serial_values = read_grid(serial);
    for (uint64_t row = 0; row < 5; ++row) {
        for (uint64_t column = 0; column < 6; ++column) {
            const double x = world_x(reference_gt, column, row);
            const double y = world_y(reference_gt, column, row);
            assert(close(serial_values[static_cast<size_t>(row * 6 + column)],
                         2.0 * x - 3.0 * y + 5.0));
        }
    }
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    require_ok(dt_grid_get_info(serial, &info), "resampled info");
    assert(info.width == 6 && info.height == 5);
    assert(info.generation == 2);
    for (int i = 0; i < 6; ++i)
        assert(close(info.geo_transform[i], reference_gt[i]));

    options.worker_count = 4;
    options.tile_row_count = 1;
    dt_grid_handle parallel = nullptr;
    require_ok(dt_grid_resample_like(source, reference, &options, &parallel),
               "parallel rotated resample");
    assert(read_grid(parallel) == serial_values);

    dt_grid_destroy(parallel);
    dt_grid_destroy(serial);
    dt_grid_destroy(reference);
    dt_grid_destroy(source);
}

void test_nearest_outside_and_nodata() {
    const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    dt_grid_handle source = make_grid(
        2, 2, gt, {0.0, 2.0, 4.0, -9999.0}, true, -9999.0);
    const double center_gt[6] = {0.5, 1.0, 0.0, 0.5, 0.0, 1.0};
    dt_grid_handle center = make_grid(1, 1, center_gt);

    dt_grid_resample_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_RESAMPLE_BILINEAR;
    options.output_nodata_value = -12345.0;
    dt_grid_handle strict = nullptr;
    require_ok(dt_grid_resample_like(source, center, &options, &strict),
               "strict NoData resample");
    assert(read_grid(strict)[0] == -12345.0);

    options.flags = DT_GRID_RESAMPLE_RENORMALIZE_NODATA;
    dt_grid_handle renormalized = nullptr;
    require_ok(dt_grid_resample_like(source, center, &options,
                                     &renormalized),
               "renormalized NoData resample");
    assert(close(read_grid(renormalized)[0], 2.0));

    const double nearest_gt[6] = {0.51, 1.0, 0.0, 0.49, 0.0, 1.0};
    dt_grid_handle nearest_reference = make_grid(1, 1, nearest_gt);
    options.method = DT_GRID_RESAMPLE_NEAREST;
    options.flags = 0;
    dt_grid_handle nearest = nullptr;
    require_ok(dt_grid_resample_like(source, nearest_reference, &options,
                                     &nearest),
               "nearest resample");
    assert(close(read_grid(nearest)[0], 2.0));

    const double outside_gt[6] = {10.0, 1.0, 0.0, 10.0, 0.0, 1.0};
    dt_grid_handle outside_reference = make_grid(1, 1, outside_gt);
    dt_grid_handle outside = nullptr;
    require_ok(dt_grid_resample_like(source, outside_reference, &options,
                                     &outside),
               "outside resample");
    assert(read_grid(outside)[0] == -12345.0);

    dt_grid_destroy(outside);
    dt_grid_destroy(outside_reference);
    dt_grid_destroy(nearest);
    dt_grid_destroy(nearest_reference);
    dt_grid_destroy(renormalized);
    dt_grid_destroy(strict);
    dt_grid_destroy(center);
    dt_grid_destroy(source);
}

void test_async_validation_and_cancel() {
    const double source_gt[6] = {0.0, 2.0, 0.0, 0.0, 0.0, 2.0};
    const double reference_gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    dt_grid_handle source = make_grid(3, 3, source_gt,
                                      {0, 2, 4, 2, 4, 6, 4, 6, 8});
    dt_grid_handle reference = make_grid(5, 5, reference_gt);
    dt_grid_resample_options options{};
    options.struct_size = sizeof(options);

    dt_task_handle task = nullptr;
    require_ok(dt_grid_resample_like_async(source, reference, &options, &task),
               "async resample start");
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed),
               "async resample wait");
    assert(completed == 1);
    dt_task_info task_info{};
    task_info.struct_size = sizeof(task_info);
    require_ok(dt_task_get_info(task, &task_info), "async resample info");
    assert(task_info.state == DT_TASK_SUCCEEDED);
    assert(task_info.result_kind == DT_TASK_RESULT_GRID);
    dt_grid_handle result = nullptr;
    require_ok(dt_task_get_grid_result(task, &result),
               "async resample result");
    assert(close(read_grid(result)[12], 4.0));
    dt_grid_destroy(result);
    dt_task_destroy(task);

    options.worker_count = 65;
    assert(dt_grid_resample_like(source, reference, &options, &result) ==
           DT_E_INVALID_ARGUMENT);
    options.worker_count = 0;
    options.flags = 8;
    assert(dt_grid_resample_like(source, reference, &options, &result) ==
           DT_E_INVALID_ARGUMENT);
    options.flags = 0;

    dt_grid_handle mismatched = make_grid(5, 5, reference_gt, {}, false,
                                          -9999.0, "OTHER");
    assert(dt_grid_resample_like(source, mismatched, &options, &result) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(mismatched);

    dt_grid_handle large_source = make_grid(8192, 1024, reference_gt);
    dt_grid_handle large_reference = make_grid(8192, 1024, reference_gt);
    require_ok(dt_grid_resample_like_async(large_source, large_reference,
                                           &options, &task),
               "cancel resample start");
    require_ok(dt_task_request_cancel(task), "cancel resample request");
    require_ok(dt_task_wait(task, UINT32_MAX, &completed),
               "cancel resample wait");
    task_info = {};
    task_info.struct_size = sizeof(task_info);
    require_ok(dt_task_get_info(task, &task_info), "cancel resample info");
    assert(task_info.state == DT_TASK_CANCELLED);
    assert(task_info.result_status == DT_E_CANCELLED);
    dt_task_destroy(task);
    dt_grid_destroy(large_reference);
    dt_grid_destroy(large_source);
    dt_grid_destroy(reference);
    dt_grid_destroy(source);
}

} // namespace

int main() {
    static_assert(sizeof(dt_grid_resample_options) == 64);
    test_rotated_bilinear_and_parallel();
    test_nearest_outside_and_nodata();
    test_async_validation_and_cancel();
    std::cout << "All GRID resampling tests passed.\n";
    return 0;
}
