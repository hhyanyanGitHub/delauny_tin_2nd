#include "dt_gdal_api.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    char detail[1024]{};
    dt_get_last_error(detail, sizeof(detail), nullptr);
    std::cerr << message;
    if (*detail) std::cerr << ": " << detail;
    std::cerr << '\n';
    std::abort();
}

void require_ok(dt_status status, const char* operation) {
    if (status != DT_OK) fail(operation);
}

void check(bool condition, const char* message) {
    if (!condition) fail(message);
}

#if DT_WITH_GDAL
void test_crs_and_reprojection() {
    require_ok(dt_gdal_initialize(), "GDAL initialization");
    size_t normalized_size = 0;
    require_ok(dt_crs_normalize_wkt("EPSG:4326", nullptr, 0,
                                    &normalized_size),
               "normalize EPSG CRS size");
    check(normalized_size > 32, "normalized CRS is unexpectedly empty");
    std::vector<char> normalized(normalized_size);
    require_ok(dt_crs_normalize_wkt("EPSG:4326", normalized.data(),
                                    normalized.size(), nullptr),
               "normalize EPSG CRS");
    int32_t same = 0;
    require_ok(dt_crs_is_same("OGC:CRS84", normalized.data(), &same),
               "compare equivalent CRS");
    check(same == 1, "equivalent CRS were not recognized");

    const dt_point3 geographic[]{{0.0, 0.0, 5.0},
                                 {116.0, 40.0, 10.0}};
    dt_point3 projected[2]{};
    require_ok(dt_crs_transform_points("EPSG:4326", "EPSG:3857",
                                       geographic, 2, projected),
               "transform CRS points");
    check(std::abs(projected[0].x) < 1.0e-9 &&
              std::abs(projected[0].y) < 1.0e-9,
          "Web Mercator origin is incorrect");
    check(projected[1].x > 1.2e7 && projected[1].y > 4.0e6,
          "traditional GIS XY axis order was not used");
    dt_point3 atomic_output{7.0, 8.0, 9.0};
    const dt_point3 invalid{std::numeric_limits<double>::quiet_NaN(), 0.0,
                            0.0};
    check(dt_crs_transform_points("EPSG:4326", "EPSG:3857", &invalid, 1,
                                  &atomic_output) == DT_E_INVALID_ARGUMENT,
          "non-finite CRS point was accepted");
    check(atomic_output.x == 7.0 && atomic_output.y == 8.0 &&
              atomic_output.z == 9.0,
          "failed point transformation modified output");

    dt_handle geographic_tin = nullptr;
    require_ok(dt_create(nullptr, &geographic_tin), "create geographic TIN");
    const dt_point3 tin_points[]{{116.0, 40.0, 100.0},
                                 {116.02, 40.0, 102.0},
                                 {116.0, 39.98, 104.0},
                                 {116.02, 39.98, 106.0}};
    require_ok(dt_build(geographic_tin, tin_points, 4, nullptr),
               "build geographic TIN");
    require_ok(dt_set_crs_wkt(geographic_tin, "EPSG:4326"),
               "set geographic TIN CRS");
    dt_handle projected_tin = nullptr;
    require_ok(dt_tin_reproject_gdal(geographic_tin, "EPSG:3857",
                                     &projected_tin),
               "reproject TIN");
    dt_statistics projected_tin_statistics{};
    projected_tin_statistics.struct_size = sizeof(projected_tin_statistics);
    require_ok(dt_get_statistics(projected_tin, &projected_tin_statistics),
               "projected TIN statistics");
    check(projected_tin_statistics.vertex_count == 4 &&
              projected_tin_statistics.bounds.xmin > 1.2e7,
          "projected TIN is invalid");

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.flags = DT_GRID_HAS_NODATA;
    create.width = 3;
    create.height = 3;
    create.geo_transform[0] = 116.0;
    create.geo_transform[1] = 0.01;
    create.geo_transform[3] = 40.0;
    create.geo_transform[5] = -0.01;
    create.nodata_value = -9999.0;
    dt_grid_handle geographic_grid = nullptr;
    require_ok(dt_grid_create(&create, &geographic_grid),
               "create geographic GRID");
    const double values[]{100.0, 101.0, 102.0,
                          103.0, 104.0, 105.0,
                          106.0, 107.0, 108.0};
    require_ok(dt_grid_write_window(geographic_grid, 0, 0, 3, 3, values, 3),
               "write geographic GRID");
    require_ok(dt_grid_set_crs_wkt(geographic_grid, "EPSG:4326"),
               "set geographic GRID CRS");

    dt_gdal_reproject_options automatic{};
    automatic.struct_size = sizeof(automatic);
    automatic.target_crs = "EPSG:3857";
    automatic.resample_algorithm = DT_GDAL_RESAMPLE_BILINEAR;
    dt_grid_handle projected_grid = nullptr;
    require_ok(dt_grid_reproject_gdal(geographic_grid, &automatic,
                                      &projected_grid),
               "automatically reproject GRID");
    dt_grid_info projected_info{};
    require_ok(dt_grid_get_info(projected_grid, &projected_info),
               "projected GRID info");
    check(projected_info.width > 0 && projected_info.height > 0,
          "projected GRID is empty");
    check(projected_info.bounds.xmin > 1.2e7,
          "projected GRID bounds are incorrect");
    size_t projected_crs_size = 0;
    require_ok(dt_grid_get_crs_wkt(projected_grid, nullptr, 0,
                                   &projected_crs_size),
               "projected GRID CRS size");
    std::vector<char> projected_crs(projected_crs_size);
    require_ok(dt_grid_get_crs_wkt(projected_grid, projected_crs.data(),
                                   projected_crs.size(), nullptr),
               "projected GRID CRS");
    require_ok(dt_crs_is_same(projected_crs.data(), "EPSG:3857", &same),
               "projected GRID CRS equivalence");
    check(same == 1, "projected GRID CRS metadata is incorrect");

    dt_gdal_reproject_options exact_back{};
    exact_back.struct_size = sizeof(exact_back);
    exact_back.flags = DT_GDAL_REPROJECT_EXPLICIT_GRID;
    exact_back.target_crs = "EPSG:4326";
    exact_back.width = 3;
    exact_back.height = 3;
    std::memcpy(exact_back.geo_transform, create.geo_transform,
                sizeof(create.geo_transform));
    dt_grid_handle roundtrip = nullptr;
    require_ok(dt_grid_reproject_gdal(projected_grid, &exact_back,
                                      &roundtrip),
               "reproject GRID to exact geometry");
    double roundtrip_values[9]{};
    require_ok(dt_grid_read_window(roundtrip, 0, 0, 3, 3,
                                   roundtrip_values, 3),
               "read reprojected GRID");
    check(std::isfinite(roundtrip_values[4]),
          "GRID roundtrip lost its center value");

    dt_contour_options contour_options{};
    contour_options.struct_size = sizeof(contour_options);
    contour_options.interval = 2.0;
    contour_options.base = 100.0;
    dt_contour_handle contours = nullptr;
    require_ok(dt_contours_from_grid(geographic_grid, &contour_options,
                                     &contours),
               "create geographic contours");
    dt_contour_handle projected_contours = nullptr;
    require_ok(dt_contours_reproject_gdal(contours, "EPSG:3857",
                                          &projected_contours),
               "reproject contours");
    dt_contour_info contour_info{};
    require_ok(dt_contours_get_info(projected_contours, &contour_info),
               "projected contour info");
    check(contour_info.line_count > 0 && contour_info.bounds.xmin > 1.2e7,
          "projected contours are invalid");

    dt_contours_destroy(projected_contours);
    dt_contours_destroy(contours);
    dt_grid_destroy(roundtrip);
    dt_grid_destroy(projected_grid);
    dt_grid_destroy(geographic_grid);
    dt_destroy(projected_tin);
    dt_destroy(geographic_tin);
}
#else
void test_crs_and_reprojection() {
    check(dt_gdal_initialize() == DT_E_UNSUPPORTED,
          "non-GDAL build reported GDAL support");
    int32_t same = 0;
    check(dt_crs_is_same("EPSG:4326", "EPSG:4326", &same) ==
              DT_E_UNSUPPORTED,
          "non-GDAL CRS comparison did not return unsupported");
    dt_point3 point{};
    check(dt_crs_transform_points("EPSG:4326", "EPSG:3857", &point, 1,
                                  &point) == DT_E_UNSUPPORTED,
          "non-GDAL point transformation did not return unsupported");
}
#endif

} // namespace

int main() {
    test_crs_and_reprojection();
    std::cout << "CRS/reprojection tests passed\n";
    return 0;
}
