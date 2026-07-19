#include "dt_cdt_api.h"
#include "dt_terrain_api.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed: " << status << " " << error << "\n";
    std::abort();
}

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "requirement failed: " << message << "\n";
    std::abort();
}

bool close(double a, double b, double tolerance = 1e-8) {
    return std::abs(a - b) <= tolerance *
           std::max({1.0, std::abs(a), std::abs(b)});
}

void verify_clip_result(dt_surface_clip_result result) {
    dt_surface_clip_result_view view{};
    require_ok(dt_surface_clip_result_get_view(result, &view),
               "read exact clip view");
    require(view.struct_size == sizeof(view) && view.source_triangle_count == 2,
            "exact clip reports source topology");
    require(close(view.exact_plan_area, 12.0),
            "outer-minus-hole plan area is exact");
    require(view.piece_count > 0 && view.ring_count >= view.piece_count &&
                view.point_count >= view.ring_count * 3,
            "exact clip returns piece/ring/point hierarchy");
    bool saw_hole = false;
    for (uint64_t i = 0; i < view.ring_count; ++i) {
        const auto& ring = view.rings[i];
        saw_hole |= ring.is_hole != 0;
        require(ring.first_point + ring.point_count <= view.point_count,
                "clip ring offsets are valid");
    }
    /* A hole may be split into boundary notches by the source diagonal, so
       area is the authoritative hole-preservation invariant. */
    (void)saw_hole;
    for (uint64_t i = 0; i < view.point_count; ++i) {
        const auto& point = view.points[i];
        require(close(point.z, point.x + 2.0 * point.y),
                "clipped XYZ remains on its source triangle plane");
    }
}

void test_exact_tin_and_cdt_clip() {
    const dt_point3 terrain[] = {{0, 0, 0}, {4, 0, 4},
                                 {4, 4, 12}, {0, 4, 8}};
    const dt_point3 polygon_points[] = {
        {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0},
        {1, 1, 0}, {1, 3, 0}, {3, 3, 0}, {3, 1, 0}};
    const uint64_t offsets[] = {0, 4, 8};
    dt_polygon_rings polygon{};
    polygon.struct_size = sizeof(polygon);
    polygon.points = polygon_points;
    polygon.point_count = 8;
    polygon.ring_offsets = offsets;
    polygon.ring_count = 2;

    dt_handle tin = nullptr;
    require_ok(dt_create(nullptr, &tin), "create clip TIN");
    require_ok(dt_build(tin, terrain, 4, nullptr), "build clip TIN");
    dt_surface_clip_result clipped = nullptr;
    require_ok(dt_tin_clip_polygon_exact(tin, &polygon, &clipped),
               "exact TIN clip");
    verify_clip_result(clipped);
    dt_surface_clip_result_destroy(clipped);

    dt_cdt_handle cdt = nullptr;
    require_ok(dt_cdt_create(nullptr, &cdt), "create clip CDT");
    require_ok(dt_cdt_build(cdt, terrain, 4), "build clip CDT");
    require_ok(dt_cdt_clip_polygon_exact(cdt, &polygon, &clipped),
               "exact CDT clip");
    verify_clip_result(clipped);
    dt_surface_clip_result_destroy(clipped);
    dt_cdt_destroy(cdt);
    dt_destroy(tin);
}

double surface(double x, double y) {
    return 0.08 * x * x + 0.13 * y * y + 0.05 * x * y;
}

dt_grid_handle make_surface_grid(bool moving) {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = 41;
    create.height = 41;
    create.geo_transform[1] = 0.5;
    create.geo_transform[5] = 0.5;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_create(&create, &grid), "create registration GRID");
    std::vector<double> values(create.width * create.height);
    for (uint64_t row = 0; row < create.height; ++row) {
        for (uint64_t column = 0; column < create.width; ++column) {
            const double x = 0.5 * static_cast<double>(column);
            const double y = 0.5 * static_cast<double>(row);
            values[row * create.width + column] = moving
                ? surface(x + 1.0, y - 0.5) - 2.0
                : surface(x, y);
        }
    }
    require_ok(dt_grid_write_window(grid, 0, 0, create.width, create.height,
                                    values.data(), create.width),
               "write registration GRID");
    return grid;
}

void test_registration_and_adaptive_error() {
    auto reference = make_surface_grid(false);
    auto moving = make_surface_grid(true);
    dt_surface_registration_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_SURFACE_REGISTRATION_ESTIMATE_Z_OFFSET;
    options.maximum_xy_shift = 2.0;
    options.minimum_xy_step = 0.125;
    options.sample_budget = 4096;
    options.minimum_valid_samples = 512;
    dt_surface_registration_result registration{};
    require_ok(dt_grid_register_surface(reference, moving, &options,
                                        &registration),
               "register GRID surfaces");
    require(close(registration.dx, 1.0, 1e-5) &&
                close(registration.dy, -0.5, 1e-5) &&
                close(registration.dz, 2.0, 1e-5),
            "known XYZ translation is recovered");
    require(registration.rmse_after < 1e-9 &&
                registration.rmse_before > registration.rmse_after &&
                registration.overlap_ratio > 0.8,
            "registration improves surface agreement");

    dt_surface_error_options error_options{};
    error_options.struct_size = sizeof(error_options);
    error_options.flags = DT_SURFACE_ERROR_APPLY_REGISTRATION;
    error_options.minimum_samples = 1024;
    error_options.maximum_samples = 16384;
    error_options.target_rmse_standard_error = 1e-8;
    error_options.dx = registration.dx;
    error_options.dy = registration.dy;
    error_options.dz = registration.dz;
    dt_surface_error_result error{};
    require_ok(dt_grid_compare_surface_adaptive(reference, moving,
                                                &error_options, &error),
               "adaptive registered error");
    require(error.valid_sample_count >= 1024 &&
                error.attempted_sample_count < 16384 && error.rmse < 1e-9 &&
                error.maximum_absolute_error < 1e-8 &&
                (error.flags & DT_SURFACE_ERROR_CONVERGED) != 0,
            "adaptive error reaches its confidence stop");

    dt_grid_handle aligned = nullptr;
    require_ok(dt_grid_apply_registration(moving, reference, &registration,
                                          &aligned),
               "apply GRID registration");
    std::vector<double> center(1);
    require_ok(dt_grid_read_window(aligned, 20, 20, 1, 1,
                                   center.data(), 1),
               "read aligned GRID center");
    require(close(center[0], surface(10.0, 10.0), 1e-8),
            "registered GRID is resampled onto reference geometry");
    dt_grid_destroy(aligned);
    dt_grid_destroy(moving);
    dt_grid_destroy(reference);
}

} // namespace

int main() {
    static_assert(sizeof(dt_polygon_rings) == 56);
    static_assert(sizeof(dt_surface_clip_result_view) == 80);
    static_assert(sizeof(dt_surface_registration_options) == 64);
    static_assert(sizeof(dt_surface_registration_result) == 96);
    static_assert(sizeof(dt_surface_error_options) == 80);
    static_assert(sizeof(dt_surface_error_result) == 104);
    test_exact_tin_and_cdt_clip();
    test_registration_and_adaptive_error();
    std::cout << "v0.85 surface operation tests passed\n";
    return 0;
}
