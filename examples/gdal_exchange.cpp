#include "dt_gdal_api.h"

#include <iostream>

int main() {
    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = 3;
    create.height = 3;
    create.geo_transform[0] = 500000.0;
    create.geo_transform[1] = 10.0;
    create.geo_transform[3] = 3200000.0;
    create.geo_transform[5] = -10.0;

    dt_grid_handle grid = nullptr;
    if (dt_grid_create(&create, &grid) != DT_OK) return 1;
    const double elevations[] = {100, 101, 102, 103, 104,
                                 105, 106, 107, 108};
    if (dt_grid_write_window(grid, 0, 0, 3, 3, elevations, 3) != DT_OK) {
        dt_grid_destroy(grid);
        return 1;
    }
    if (dt_grid_set_crs_wkt(grid, "EPSG:32650") != DT_OK) {
        dt_grid_destroy(grid);
        return 1;
    }

    dt_gdal_reproject_options reproject{};
    reproject.struct_size = sizeof(reproject);
    reproject.target_crs = "EPSG:4326";
    reproject.resample_algorithm = DT_GDAL_RESAMPLE_BILINEAR;
    dt_grid_handle geographic = nullptr;
    if (dt_grid_reproject_gdal(grid, &reproject, &geographic) != DT_OK) {
        char error[512]{};
        dt_get_last_error(error, sizeof(error), nullptr);
        std::cerr << error << '\n';
        dt_grid_destroy(grid);
        return 1;
    }

    dt_gdal_raster_save_options save{};
    save.struct_size = sizeof(save);
    save.driver_name = "COG";
    const char* creation_options[] = {"COMPRESS=DEFLATE", nullptr};
    save.creation_options = creation_options;
    const dt_status status =
        dt_grid_save_gdal_raster(geographic, "sample_terrain.tif", &save);
    dt_grid_destroy(geographic);
    dt_grid_destroy(grid);
    if (status != DT_OK) {
        char error[512]{};
        dt_get_last_error(error, sizeof(error), nullptr);
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Reprojected EPSG:32650 to EPSG:4326 and wrote "
                 "sample_terrain.tif\n";
    return 0;
}
