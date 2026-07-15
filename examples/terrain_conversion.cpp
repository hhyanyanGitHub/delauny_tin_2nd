#include "dt_api.h"
#include "dt_task_api.h"
#include "dt_terrain_api.h"

#include <cstdint>
#include <iostream>

namespace {

bool check(dt_status status, const char* operation) {
    if (status == DT_OK) return true;
    char message[512]{};
    dt_get_last_error(message, sizeof(message), nullptr);
    std::cerr << operation << " failed: " << message << '\n';
    return false;
}

} // namespace

int main() {
    dt_handle tin = nullptr;
    if (!check(dt_create(nullptr, &tin), "dt_create")) return 1;
    const dt_point3 points[] = {
        {0.0, 0.0, 100.0}, {100.0, 0.0, 120.0},
        {100.0, 100.0, 140.0}, {0.0, 100.0, 120.0}};
    if (!check(dt_build(tin, points, 4, nullptr), "dt_build")) {
        dt_destroy(tin);
        return 2;
    }

    dt_tin_to_grid_options grid_options{};
    grid_options.struct_size = sizeof(grid_options);
    grid_options.width = 101;
    grid_options.height = 101;
    grid_options.geo_transform[1] = 1.0;
    grid_options.geo_transform[5] = 1.0;
    grid_options.nodata_value = -9999.0;

    dt_task_handle task = nullptr;
    if (!check(dt_grid_from_tin_async(tin, &grid_options, &task),
               "dt_grid_from_tin_async")) {
        dt_destroy(tin);
        return 3;
    }
    int32_t completed = 0;
    check(dt_task_wait(task, UINT32_MAX, &completed), "dt_task_wait");
    dt_grid_handle grid = nullptr;
    if (!completed ||
        !check(dt_task_get_grid_result(task, &grid), "grid result")) {
        char message[512]{};
        dt_task_get_error(task, message, sizeof(message), nullptr);
        std::cerr << "conversion failed: " << message << '\n';
        dt_task_destroy(task);
        dt_destroy(tin);
        return 4;
    }
    dt_task_destroy(task);

    dt_contour_options contour_options{};
    contour_options.struct_size = sizeof(contour_options);
    contour_options.interval = 5.0;
    contour_options.base = 0.0;
    dt_contour_handle contours = nullptr;
    if (!check(dt_contours_from_grid(grid, &contour_options, &contours),
               "dt_contours_from_grid")) {
        dt_grid_destroy(grid);
        dt_destroy(tin);
        return 5;
    }

    dt_grid_info grid_info{};
    dt_contour_info contour_info{};
    dt_grid_get_info(grid, &grid_info);
    dt_contours_get_info(contours, &contour_info);
    std::cout << "grid=" << grid_info.width << 'x' << grid_info.height
              << ", valid=" << grid_info.valid_value_count
              << ", contour lines=" << contour_info.line_count << '\n';

    check(dt_grid_save_text(grid, "terrain_example.dgrid"), "save GRID");
    check(dt_contours_save_text(contours, "terrain_example.dcontour"),
          "save contours");

    dt_contours_destroy(contours);
    dt_grid_destroy(grid);
    dt_destroy(tin);
    return 0;
}
