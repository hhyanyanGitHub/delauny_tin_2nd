#ifndef DT_TERRAIN_API_H
#define DT_TERRAIN_API_H

#include "dt_api.h"

#ifdef __cplusplus
extern "C" {
#endif

enum dt_grid_flags {
    DT_GRID_HAS_NODATA = 1u << 0
};

enum dt_grid_to_tin_flags {
    /* Omits NoData nodes. Ordinary TIN construction may bridge holes; use a
       separate dt_cdt_handle when hard hole boundaries are required. */
    DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING = 1u << 0
};

typedef struct dt_grid_create_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t width;
    uint64_t height;
    /* Node (column,row) maps to:
       X=gt[0]+column*gt[1]+row*gt[2]
       Y=gt[3]+column*gt[4]+row*gt[5]. */
    double geo_transform[6];
    double nodata_value;
    uint64_t reserved[4];
} dt_grid_create_options;

typedef struct dt_grid_info {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t width;
    uint64_t height;
    double geo_transform[6];
    double nodata_value;
    dt_bounds2 bounds;
    uint64_t valid_value_count;
    uint64_t generation;
} dt_grid_info;

typedef struct dt_tin_to_grid_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t width;
    uint64_t height;
    double geo_transform[6];
    double nodata_value;
    uint64_t reserved[4];
} dt_tin_to_grid_options;

typedef struct dt_grid_to_tin_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t reserved[6];
} dt_grid_to_tin_options;

typedef struct dt_contours_to_tin_options {
    uint32_t struct_size;
    uint32_t flags;
    /* Zero keeps source vertices only. Positive values add samples so no
       source contour segment is longer than this XY distance. */
    double maximum_segment_length;
    /* Zero merges exact duplicate XY only. Positive values merge vertices
       within this XY distance when their contour elevations agree. */
    double merge_tolerance;
    uint64_t reserved[4];
} dt_contours_to_tin_options;

typedef struct dt_contour_options {
    uint32_t struct_size;
    uint32_t flags;
    /* Used when level_count is zero. */
    double interval;
    double base;
    /* Optional explicit levels. The memory is only borrowed for the call. */
    const double* levels;
    uint64_t level_count;
    /* Zero selects an extent-dependent default. */
    double stitch_tolerance;
    uint64_t reserved[4];
} dt_contour_options;

typedef struct dt_contour_info {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t line_count;
    uint64_t vertex_count;
    double minimum_elevation;
    double maximum_elevation;
    dt_bounds2 bounds;
} dt_contour_info;

enum dt_contour_line_flags {
    DT_CONTOUR_LINE_CLOSED = 1u << 0
};

typedef struct dt_contour_line_view {
    uint32_t struct_size;
    uint32_t flags;
    double elevation;
    const dt_point3* points;
    uint64_t point_count;
} dt_contour_line_view;

typedef struct dt_grid_t* dt_grid_handle;
typedef struct dt_contour_set_t* dt_contour_handle;

DT_API dt_status DT_CALL dt_grid_create(
    const dt_grid_create_options* options, dt_grid_handle* output_grid);
DT_API void DT_CALL dt_grid_destroy(dt_grid_handle grid);
DT_API dt_status DT_CALL dt_grid_get_info(
    dt_grid_handle grid, dt_grid_info* output_info);
DT_API dt_status DT_CALL dt_grid_set_crs_wkt(
    dt_grid_handle grid, const char* utf8_crs_wkt);
DT_API dt_status DT_CALL dt_grid_get_crs_wkt(
    dt_grid_handle grid, char* buffer, size_t buffer_size,
    size_t* required_size);

/* row_stride is measured in doubles; zero means width. */
DT_API dt_status DT_CALL dt_grid_read_window(
    dt_grid_handle grid, uint64_t column, uint64_t row,
    uint64_t width, uint64_t height, double* output_values,
    uint64_t row_stride);
DT_API dt_status DT_CALL dt_grid_write_window(
    dt_grid_handle grid, uint64_t column, uint64_t row,
    uint64_t width, uint64_t height, const double* values,
    uint64_t row_stride);

/* DGRID 1 is a portable UTF-8 text format intended for tests and exchange. */
DT_API dt_status DT_CALL dt_grid_save_text(
    dt_grid_handle grid, const char* utf8_file_name);
DT_API dt_status DT_CALL dt_grid_load_text(
    const char* utf8_file_name, dt_grid_handle* output_grid);

/* Samples the piecewise-linear TIN surface at each output grid node. */
DT_API dt_status DT_CALL dt_grid_from_tin(
    dt_handle tin, const dt_tin_to_grid_options* options,
    dt_grid_handle* output_grid);

/* Replaces output_tin atomically. NoData bridging requires explicit opt-in;
   this ordinary-TIN function does not infer CDT hole boundaries. */
DT_API dt_status DT_CALL dt_tin_from_grid(
    dt_grid_handle grid, const dt_grid_to_tin_options* options,
    dt_handle output_tin);

/* Replaces output_tin atomically with an ordinary Delaunay TIN sampled from
   contour vertices. LINE elevation is authoritative. Contour polylines are
   not forced to become TIN edges; use a CDT when hard constraints are needed. */
DT_API dt_status DT_CALL dt_tin_from_contours(
    dt_contour_handle contours,
    const dt_contours_to_tin_options* options, dt_handle output_tin);

/* Builds the same intermediate contour-sampled TIN and samples it at the
   requested GRID nodes. */
DT_API dt_status DT_CALL dt_grid_from_contours(
    dt_contour_handle contours,
    const dt_contours_to_tin_options* tin_options,
    const dt_tin_to_grid_options* grid_options,
    dt_grid_handle* output_grid);

DT_API dt_status DT_CALL dt_contours_from_tin(
    dt_handle tin, const dt_contour_options* options,
    dt_contour_handle* output_contours);
DT_API dt_status DT_CALL dt_contours_from_grid(
    dt_grid_handle grid, const dt_contour_options* options,
    dt_contour_handle* output_contours);
DT_API void DT_CALL dt_contours_destroy(dt_contour_handle contours);
DT_API dt_status DT_CALL dt_contours_get_info(
    dt_contour_handle contours, dt_contour_info* output_info);
DT_API dt_status DT_CALL dt_contours_get_line(
    dt_contour_handle contours, uint64_t line_index,
    dt_contour_line_view* output_line);
DT_API dt_status DT_CALL dt_contours_set_crs_wkt(
    dt_contour_handle contours, const char* utf8_crs_wkt);
DT_API dt_status DT_CALL dt_contours_get_crs_wkt(
    dt_contour_handle contours, char* buffer, size_t buffer_size,
    size_t* required_size);

/* DCONTOUR 1 stores one LINE record followed by its XYZ vertices. */
DT_API dt_status DT_CALL dt_contours_save_text(
    dt_contour_handle contours, const char* utf8_file_name);
DT_API dt_status DT_CALL dt_contours_load_text(
    const char* utf8_file_name, dt_contour_handle* output_contours);

#ifdef __cplusplus
}
#endif

#endif
