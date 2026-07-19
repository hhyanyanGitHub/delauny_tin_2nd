#ifndef DT_GDAL_IO_HPP
#define DT_GDAL_IO_HPP

#include "dt_gdal_api.h"
#include "dt_terrain_core.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace dt {

void gdal_initialize();
bool gdal_driver_available(const char* driver_name);
std::string normalize_crs_wkt(const char* definition);
bool crs_is_same(const char* first, const char* second);
void transform_points(const char* source_crs, const char* target_crs,
                      const dt_point3* input, uint64_t count,
                      dt_point3* output);

std::unique_ptr<Grid> grid_load_gdal(
    const char* file_name, const dt_gdal_raster_load_options& options);
void grid_save_gdal(const Grid& grid, const char* file_name,
                    const dt_gdal_raster_save_options& options);
std::unique_ptr<Grid> grid_reproject_gdal(
    const Grid& source, const dt_gdal_reproject_options& options);

std::unique_ptr<ContourSet> contours_load_gdal(
    const char* file_name, const dt_gdal_contour_load_options& options);
void contours_save_gdal(const ContourSet& contours, const char* file_name,
                        const dt_gdal_contour_save_options& options);
std::unique_ptr<ContourSet> contours_reproject_gdal(
    const ContourSet& source, const char* target_crs);

} // namespace dt

#endif
