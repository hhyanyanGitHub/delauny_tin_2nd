#include "dt_api.h"
#include "dt_task_api.h"
#include "dt_terrain_api.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed with status " << status << ": "
              << error << '\n';
    std::abort();
}

bool close(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <=
           tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

dt_grid_handle make_grid(uint64_t width, uint64_t height,
                         const double* transform,
                         const std::vector<double>& values,
                         bool has_nodata = false) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = has_nodata ? DT_GRID_HAS_NODATA : 0;
    create.width = width;
    create.height = height;
    std::copy(transform, transform + 6, create.geo_transform);
    create.nodata_value = -9999.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create earthwork GRID");
    if (!values.empty()) {
        require_ok(dt_grid_write_window(grid, 0, 0, width, height,
                                        values.data(), width),
                   "write earthwork GRID");
    }
    return grid;
}

void test_constant_and_difference_grid() {
    const double transform[] = {100.0, 2.0, 0.0, 200.0, 0.0, 3.0};
    auto existing = make_grid(3, 3, transform, std::vector<double>(9, 10.0));
    auto design = make_grid(3, 3, transform, std::vector<double>(9, 8.0));
    dt_grid_earthwork_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID;
    options.worker_count = 4;
    options.tile_row_count = 1;
    dt_grid_earthwork_result result{};
    dt_grid_handle difference = nullptr;
    require_ok(dt_grid_compare_earthwork(existing, design, &options,
                                          &result, &difference),
               "constant earthwork comparison");
    assert(result.struct_size == sizeof(result));
    assert(result.cell_count == 4);
    assert(result.valid_triangle_count == 8);
    assert(result.skipped_triangle_count == 0);
    assert(close(result.total_plan_area, 24.0));
    assert(close(result.valid_plan_area, 24.0));
    assert(close(result.coverage_ratio, 1.0));
    assert(close(result.cut_volume, 48.0));
    assert(close(result.fill_volume, 0.0));
    assert(close(result.net_volume, 48.0));
    assert(close(result.minimum_difference, 2.0));
    assert(close(result.maximum_difference, 2.0));
    assert(close(result.mean_difference, 2.0));
    assert(close(result.rmse_difference, 2.0));
    std::vector<double> values(9);
    require_ok(dt_grid_read_window(difference, 0, 0, 3, 3,
                                   values.data(), 3),
               "read difference GRID");
    for (double value : values) assert(close(value, 2.0));
    dt_grid_info info{};
    require_ok(dt_grid_get_info(difference, &info), "difference GRID info");
    assert(info.generation == 2);
    dt_grid_destroy(difference);
    dt_grid_destroy(existing);
    dt_grid_destroy(design);
}

void test_zero_crossing_and_nodata() {
    const double transform[] = {0, 1, 0, 0, 0, 1};
    auto existing = make_grid(2, 2, transform, {1, -1, -1, 1});
    auto design = make_grid(2, 2, transform, {0, 0, 0, 0});
    dt_grid_earthwork_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = 1;
    dt_grid_earthwork_result result{};
    require_ok(dt_grid_compare_earthwork(existing, design, &options,
                                          &result, nullptr),
               "crossing earthwork comparison");
    assert(close(result.cut_volume, 5.0 / 12.0));
    assert(close(result.fill_volume, 1.0 / 12.0));
    assert(close(result.net_volume, 1.0 / 3.0));
    assert(close(result.mean_difference, 1.0 / 3.0));
    assert(close(result.rmse_difference, std::sqrt(1.0 / 3.0)));
    dt_grid_destroy(existing);
    dt_grid_destroy(design);

    existing = make_grid(2, 2, transform, {1, -9999, 1, 1}, true);
    design = make_grid(2, 2, transform, {0, 0, 0, 0});
    require_ok(dt_grid_compare_earthwork(existing, design, &options,
                                          &result, nullptr),
               "strict NoData comparison");
    assert(result.valid_triangle_count == 0);
    assert(result.skipped_triangle_count == 2);
    assert(close(result.coverage_ratio, 0.0));
    assert(std::isnan(result.mean_difference));
    options.flags = DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS;
    require_ok(dt_grid_compare_earthwork(existing, design, &options,
                                          &result, nullptr),
               "partial NoData comparison");
    assert(result.valid_triangle_count == 1);
    assert(result.skipped_triangle_count == 1);
    assert(close(result.coverage_ratio, 0.5));
    assert(close(result.cut_volume, 0.5));
    dt_grid_destroy(existing);
    dt_grid_destroy(design);
}

void test_rotated_async_and_cancel() {
    const double transform[] = {500000, 2, 1, 3200000, -1, 3};
    auto existing = make_grid(2, 2, transform, {5, 5, 5, 5});
    auto design = make_grid(2, 2, transform, {3, 3, 3, 3});
    dt_grid_earthwork_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID;
    dt_task_handle task = nullptr;
    require_ok(dt_grid_compare_earthwork_async(existing, design, &options,
                                                &task),
               "async earthwork start");
    int32_t completed = 0;
    require_ok(dt_task_wait(task, UINT32_MAX, &completed),
               "async earthwork wait");
    dt_task_info info{};
    require_ok(dt_task_get_info(task, &info), "async earthwork info");
    assert(info.state == DT_TASK_SUCCEEDED);
    assert(info.result_kind == DT_TASK_RESULT_EARTHWORK);
    dt_grid_earthwork_result result{};
    dt_grid_handle difference = nullptr;
    require_ok(dt_task_get_earthwork_result(task, &result, &difference),
               "async earthwork result");
    assert(close(result.total_plan_area, 7.0));
    assert(close(result.cut_volume, 14.0));
    assert(difference != nullptr);
    dt_grid_destroy(difference);
    dt_task_destroy(task);
    dt_grid_destroy(existing);
    dt_grid_destroy(design);

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = 2048;
    create.height = 2048;
    create.geo_transform[1] = 1;
    create.geo_transform[5] = 1;
    require_ok(dt_grid_create(&create, &existing), "large existing GRID");
    require_ok(dt_grid_create(&create, &design), "large design GRID");
    options.flags = 0;
    options.worker_count = 4;
    options.tile_row_count = 1;
    require_ok(dt_grid_compare_earthwork_async(existing, design, &options,
                                                &task),
               "cancel earthwork start");
    require_ok(dt_task_request_cancel(task), "cancel earthwork request");
    require_ok(dt_task_wait(task, UINT32_MAX, &completed),
               "cancel earthwork wait");
    require_ok(dt_task_get_info(task, &info), "cancel earthwork info");
    assert(info.state == DT_TASK_CANCELLED);
    assert(info.result_status == DT_E_CANCELLED);
    dt_task_destroy(task);
    dt_grid_destroy(existing);
    dt_grid_destroy(design);
}

void test_validation() {
    const double transform[] = {0, 1, 0, 0, 0, 1};
    auto existing = make_grid(2, 2, transform, {1, 1, 1, 1});
    auto design = make_grid(2, 2, transform, {0, 0, 0, 0});
    dt_grid_earthwork_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = 65;
    dt_grid_earthwork_result result{};
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    options.worker_count = 0;
    options.flags = DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID;
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.tile_row_count = 1024U * 1024U + 1U;
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    options.tile_row_count = 0;
    options.existing_z_factor = -1.0;
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    options.existing_z_factor = 0.0;
    dt_grid_set_crs_wkt(existing, "LOCAL_EXISTING");
    dt_grid_set_crs_wkt(design, "LOCAL_DESIGN");
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(existing);
    dt_grid_destroy(design);

    const double shifted_transform[] = {0.25, 1, 0, 0, 0, 1};
    existing = make_grid(2, 2, transform, {1, 1, 1, 1});
    design = make_grid(2, 2, shifted_transform, {0, 0, 0, 0});
    assert(dt_grid_compare_earthwork(existing, design, &options,
                                     &result, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(existing);
    dt_grid_destroy(design);
}

} // namespace

int main() {
    static_assert(sizeof(dt_grid_earthwork_options) == 64,
                  "earthwork options ABI size changed");
    static_assert(sizeof(dt_grid_earthwork_result) == 112,
                  "earthwork result ABI size changed");
    test_constant_and_difference_grid();
    test_zero_crossing_and_nodata();
    test_rotated_async_and_cancel();
    test_validation();
    std::cout << "All earthwork tests passed.\n";
    return 0;
}
