#ifndef DT_GDAL_IO_HPP
#define DT_GDAL_IO_HPP

#include "dt_gdal_api.h"
#include "dt_terrain_core.hpp"

#include <memory>

namespace dt {

void gdal_initialize();
bool gdal_driver_available(const char* driver_name);

std::unique_ptr<Grid> grid_load_gdal(
    const char* file_name, const dt_gdal_raster_load_options& options);
void grid_save_gdal(const Grid& grid, const char* file_name,
                    const dt_gdal_raster_save_options& options);

std::unique_ptr<ContourSet> contours_load_gdal(
    const char* file_name, const dt_gdal_contour_load_options& options);
void contours_save_gdal(const ContourSet& contours, const char* file_name,
                        const dt_gdal_contour_save_options& options);

} // namespace dt

#endif
