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

enum dt_gdal_reproject_flags {
    /* Use width, height and geo_transform as the exact output node grid.
       Otherwise GDAL derives a north-up extent and resolution. */
    DT_GDAL_REPROJECT_EXPLICIT_GRID = 1u << 0
};

enum dt_gdal_resample_algorithm {
    DT_GDAL_RESAMPLE_NEAREST = 1,
    DT_GDAL_RESAMPLE_BILINEAR = 2,
    DT_GDAL_RESAMPLE_CUBIC = 3,
    DT_GDAL_RESAMPLE_CUBIC_SPLINE = 4,
    DT_GDAL_RESAMPLE_LANCZOS = 5
};

typedef struct dt_gdal_reproject_options {
    uint32_t struct_size;
    uint32_t flags;
    /* Accepts EPSG:xxxx, OGC WKT, PROJ strings and other GDAL user input. */
    const char* target_crs;
    /* Zero selects bilinear. */
    uint32_t resample_algorithm;
    uint32_t reserved0;
    uint64_t width;
    uint64_t height;
    /* Node-centered affine transform, used with EXPLICIT_GRID. */
    double geo_transform[6];
    /* Zero preserves source NoData when present, otherwise selects NaN. */
    double output_nodata_value;
    uint64_t reserved[4];
} dt_gdal_reproject_options;

/* Registers GDAL drivers. Calls are idempotent and thread-safe. */
DT_API dt_status DT_CALL dt_gdal_initialize(void);
DT_API dt_status DT_CALL dt_gdal_is_driver_available(
    const char* driver_name, int32_t* output_available);

/* Normalizes any GDAL-supported CRS definition to OGC WKT. The required byte
   count includes the trailing NUL. */
DT_API dt_status DT_CALL dt_crs_normalize_wkt(
    const char* crs_definition, char* buffer, size_t buffer_size,
    size_t* required_size);
DT_API dt_status DT_CALL dt_crs_is_same(
    const char* first_crs, const char* second_crs, int32_t* output_same);
/* Input and output may alias. Transformation is atomic: output is unchanged
   if any coordinate cannot be transformed. Traditional GIS XY axis order is
   used consistently, including for EPSG geographic CRS definitions. */
DT_API dt_status DT_CALL dt_crs_transform_points(
    const char* source_crs, const char* target_crs,
    const dt_point3* input_points, uint64_t point_count,
    dt_point3* output_points);
/* Reprojects all TIN vertices and rebuilds Delaunay topology in the target
   CRS. The returned independent handle is owned by the caller. */
DT_API dt_status DT_CALL dt_tin_reproject_gdal(
    dt_handle source, const char* target_crs, dt_handle* output_tin);

DT_API dt_status DT_CALL dt_grid_load_gdal_raster(
    const char* utf8_file_name, const dt_gdal_raster_load_options* options,
    dt_grid_handle* output_grid);
DT_API dt_status DT_CALL dt_grid_save_gdal_raster(
    dt_grid_handle grid, const char* utf8_file_name,
    const dt_gdal_raster_save_options* options);
DT_API dt_status DT_CALL dt_grid_reproject_gdal(
    dt_grid_handle source, const dt_gdal_reproject_options* options,
    dt_grid_handle* output_grid);

DT_API dt_status DT_CALL dt_contours_load_gdal_vector(
    const char* utf8_file_name, const dt_gdal_contour_load_options* options,
    dt_contour_handle* output_contours);
DT_API dt_status DT_CALL dt_contours_save_gdal_vector(
    dt_contour_handle contours, const char* utf8_file_name,
    const dt_gdal_contour_save_options* options);
DT_API dt_status DT_CALL dt_contours_reproject_gdal(
    dt_contour_handle source, const char* target_crs,
    dt_contour_handle* output_contours);

#ifdef __cplusplus
}
#endif

#endif
