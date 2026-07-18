#include "dt_terrain_api.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
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

bool close(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <=
           tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

dt_grid_handle make_grid(uint64_t width, uint64_t height,
                         bool with_nodata = false) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = with_nodata ? DT_GRID_HAS_NODATA : 0;
    create.width = width;
    create.height = height;
    create.geo_transform[1] = 1.0;
    create.geo_transform[5] = 1.0;
    create.nodata_value = -9999.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create GRID");
    std::vector<double> values(static_cast<size_t>(width * height));
    for (uint64_t row = 0; row < height; ++row)
        for (uint64_t column = 0; column < width; ++column)
            values[static_cast<size_t>(row * width + column)] =
                static_cast<double>(row * 10 + column);
    if (with_nodata) {
        values[1] = -9999.0;
        values[static_cast<size_t>(height * width - 1)] = -9999.0;
    }
    require_ok(dt_grid_write_window(grid, 0, 0, width, height,
                                    values.data(), width),
               "write GRID");
    return grid;
}

void test_aggregate_methods_and_statistics() {
    dt_grid_handle grid = make_grid(6, 4, true);
    dt_grid_overview_options options{};
    options.struct_size = sizeof(options);
    options.worker_count = 1;
    std::vector<double> values(8, 123456.0);
    dt_grid_overview_result result{};
    require_ok(dt_grid_read_overview(grid, &options, 3, 2, values.data(), 4,
                                     &result),
               "average overview");
    assert(result.flags & DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS);
    assert(result.valid_value_count == 22);
    assert(result.nodata_value_count == 2);
    assert(close(result.minimum_value, 0.0));
    assert(close(result.maximum_value, 34.0));
    assert(close(result.mean_value, 384.0 / 22.0));
    assert(close(values[0], 7.0));
    assert(close(values[1], 7.5));
    assert(close(values[2], 9.5));
    assert(values[3] == 123456.0);
    assert(close(values[4], 25.5));
    assert(close(values[5], 27.5));
    assert(close(values[6], 83.0 / 3.0));
    assert(values[7] == 123456.0);

    options.flags = DT_GRID_OVERVIEW_STRICT_NODATA;
    require_ok(dt_grid_read_overview(grid, &options, 3, 2, values.data(), 4,
                                     nullptr),
               "strict overview");
    assert(values[0] == -9999.0);
    assert(values[6] == -9999.0);

    options.flags = 0;
    options.method = DT_GRID_OVERVIEW_MINIMUM;
    require_ok(dt_grid_read_overview(grid, &options, 3, 2, values.data(), 4,
                                     nullptr),
               "minimum overview");
    assert(close(values[0], 0.0) && close(values[1], 2.0));
    assert(close(values[4], 20.0) && close(values[6], 24.0));

    options.method = DT_GRID_OVERVIEW_MAXIMUM;
    require_ok(dt_grid_read_overview(grid, &options, 3, 2, values.data(), 4,
                                     nullptr),
               "maximum overview");
    assert(close(values[0], 11.0) && close(values[2], 15.0));
    assert(close(values[4], 31.0) && close(values[6], 34.0));
    dt_grid_destroy(grid);
}

void test_nearest_subwindow_parallel_and_validation() {
    dt_grid_handle grid = make_grid(6, 4, true);
    dt_grid_overview_options options{};
    options.struct_size = sizeof(options);
    options.method = DT_GRID_OVERVIEW_NEAREST;
    options.worker_count = 4;
    options.tile_row_count = 1;
    std::vector<double> values(6);
    dt_grid_overview_result result{};
    require_ok(dt_grid_read_overview(grid, &options, 3, 2, values.data(), 0,
                                     &result),
               "nearest overview");
    assert((result.flags & DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS) == 0);
    assert(result.valid_value_count == 5 && result.nodata_value_count == 1);
    assert(close(result.minimum_value, 11.0));
    assert(close(result.maximum_value, 33.0));
    assert(close(result.mean_value, 20.6));
    const double expected[] = {11, 13, 15, 31, 33, -9999};
    for (size_t index = 0; index < values.size(); ++index)
        assert(values[index] == expected[index]);
    values.resize(12 * 8);
    require_ok(dt_grid_read_overview(grid, &options, 12, 8, values.data(), 0,
                                     nullptr),
               "nearest upsample overview");

    options.method = DT_GRID_OVERVIEW_AVERAGE;
    options.source_column = 1;
    options.source_row = 1;
    options.source_width = 4;
    options.source_height = 2;
    values.resize(2);
    require_ok(dt_grid_read_overview(grid, &options, 2, 1, values.data(), 0,
                                     &result),
               "subwindow overview");
    assert(close(values[0], 16.5) && close(values[1], 18.5));
    assert(result.valid_value_count == 8 && result.nodata_value_count == 0);
    assert(close(result.minimum_value, 11.0));
    assert(close(result.maximum_value, 24.0));
    assert(close(result.mean_value, 17.5));

    options.source_column = options.source_row = 0;
    options.source_width = options.source_height = 0;
    assert(dt_grid_read_overview(grid, &options, 7, 4, values.data(), 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    assert(dt_grid_read_overview(grid, &options, 0, 1, values.data(), 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    assert(dt_grid_read_overview(grid, &options, 2, 1, values.data(), 1,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    options.method = 99;
    assert(dt_grid_read_overview(grid, &options, 1, 1, values.data(), 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    options.method = DT_GRID_OVERVIEW_AVERAGE;
    options.flags = 8;
    assert(dt_grid_read_overview(grid, &options, 1, 1, values.data(), 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.worker_count = 65;
    assert(dt_grid_read_overview(grid, &options, 1, 1, values.data(), 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    options.worker_count = 1;
    assert(dt_grid_read_overview(grid, &options, 1, 1, nullptr, 0,
                                 nullptr) == DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(grid);

    dt_grid_handle parallel_grid = make_grid(64, 32);
    options.worker_count = 1;
    options.tile_row_count = 1;
    std::vector<double> serial(8 * 4);
    std::vector<double> parallel(serial.size());
    require_ok(dt_grid_read_overview(parallel_grid, &options, 8, 4,
                                     serial.data(), 0, nullptr),
               "serial overview");
    options.worker_count = 4;
    require_ok(dt_grid_read_overview(parallel_grid, &options, 8, 4,
                                     parallel.data(), 0, nullptr),
               "parallel overview");
    assert(serial == parallel);
    dt_grid_destroy(parallel_grid);
}

} // namespace

int main() {
    static_assert(sizeof(dt_grid_overview_options) == 64);
    static_assert(sizeof(dt_grid_overview_result) == 64);
    test_aggregate_methods_and_statistics();
    test_nearest_subwindow_parallel_and_validation();
    std::cout << "All GRID overview tests passed.\n";
    return 0;
}
