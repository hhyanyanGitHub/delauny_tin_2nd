#ifndef DT_GDAL_API_H
#define DT_GDAL_API_H

#include "dt_terrain_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_gdal_raster_load_options {
    uint32_t struct_size;
    uint32_t flags;
    /* One-based band number. Zero selects band 1. */
    uint32_t band_index;
    uint32_t reserved0;
    uint64_t reserved[4];
} dt_gdal_raster_load_options;

typedef struct dt_gdal_raster_save_options {
    uint32_t struct_size;
    uint32_t flags;
    /* NULL selects GTiff. Use "COG" for Cloud Optimized GeoTIFF. */
    const char* driver_name;
    /* Optional NULL-terminated NAME=VALUE list borrowed for the call. */
    const char* const* creation_options;
    uint64_t reserved[4];
} dt_gdal_raster_save_options;

typedef struct dt_gdal_contour_load_options {
    uint32_t struct_size;
    uint32_t flags;
    /* NULL selects the first layer. */
    const char* layer_name;
    /* NULL selects "elevation", then geometry Z as a fallback. */
    const char* elevation_field;
    uint64_t reserved[4];
} dt_gdal_contour_load_options;

typedef struct dt_gdal_contour_save_options {
    uint32_t struct_size;
    uint32_t flags;
    /* NULL selects GPKG. */
    const char* driver_name;
    /* NULL selects "contours". */
    const char* layer_name;
    /* NULL selects "elevation". */
    const char* elevation_field;
    const char* const* dataset_creation_options;
    const char* const* layer_creation_options;
    uint64_t reserved[4];
} dt_gdal_contour_save_options;

/* Registers GDAL drivers. Calls are idempotent and thread-safe. */
DT_API dt_status DT_CALL dt_gdal_initialize(void);
DT_API dt_status DT_CALL dt_gdal_is_driver_available(
    const char* driver_name, int32_t* output_available);

DT_API dt_status DT_CALL dt_grid_load_gdal_raster(
    const char* utf8_file_name, const dt_gdal_raster_load_options* options,
    dt_grid_handle* output_grid);
DT_API dt_status DT_CALL dt_grid_save_gdal_raster(
    dt_grid_handle grid, const char* utf8_file_name,
    const dt_gdal_raster_save_options* options);

DT_API dt_status DT_CALL dt_contours_load_gdal_vector(
    const char* utf8_file_name, const dt_gdal_contour_load_options* options,
    dt_contour_handle* output_contours);
DT_API dt_status DT_CALL dt_contours_save_gdal_vector(
    dt_contour_handle contours, const char* utf8_file_name,
    const dt_gdal_contour_save_options* options);

#ifdef __cplusplus
}
#endif

#endif
