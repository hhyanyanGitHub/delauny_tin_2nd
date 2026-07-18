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

enum dt_grid_terrain_kind {
    /* Rise angle from the horizontal, in degrees [0, 90]. */
    DT_GRID_TERRAIN_SLOPE_DEGREES = 1,
    /* Downslope azimuth clockwise from north, in degrees [0, 360). Flat
       cells are written as NoData because their aspect is undefined. */
    DT_GRID_TERRAIN_ASPECT_DEGREES = 2,
    /* Grayscale analytical hillshade in [0, 255]. */
    DT_GRID_TERRAIN_HILLSHADE = 3
};

enum dt_grid_earthwork_flags {
    /* Creates a node-aligned GRID containing existing-design elevation
       differences. The caller owns the returned GRID handle. */
    DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID = 1u << 0,
    /* By default both triangles of a cell are skipped when any of its four
       corner pairs is NoData. This flag evaluates each triangle separately. */
    DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS = 1u << 1
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

typedef struct dt_grid_terrain_options {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t flags;
    uint32_t reserved;
    /* Zero selects 1.0. Otherwise this must be finite and positive. */
    double z_factor;
    /* Used by hillshade. If both angles are zero, 315/45 degrees is used. */
    double sun_azimuth_degrees;
    double sun_altitude_degrees;
    /* The derived GRID always has NoData. Zero selects NaN so a zero-filled
       options structure cannot collide with valid 0-degree/0-gray results;
       otherwise this may be finite or NaN. */
    double output_nodata_value;
    /* Zero selects an implementation-defined automatic count. One forces
       deterministic single-thread execution. Values above 64 are rejected. */
    uint32_t worker_count;
    /* Rows claimed by one worker at a time. Zero selects 64 rows. */
    uint32_t tile_row_count;
    uint64_t reserved2[3];
} dt_grid_terrain_options;

typedef struct dt_grid_earthwork_options {
    uint32_t struct_size;
    uint32_t flags;
    /* Zero selects an implementation-defined automatic count. One forces
       single-thread execution. Values above 64 are rejected. */
    uint32_t worker_count;
    /* Cell rows claimed by one worker at a time. Zero selects 64 rows. */
    uint32_t tile_row_count;
    /* Zero selects 1.0. Otherwise both factors must be finite and positive. */
    double existing_z_factor;
    double design_z_factor;
    /* Used only for the optional difference GRID. Zero selects NaN. */
    double output_nodata_value;
    uint64_t reserved[3];
} dt_grid_earthwork_options;

typedef struct dt_grid_earthwork_result {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t cell_count;
    uint64_t valid_triangle_count;
    uint64_t skipped_triangle_count;
    double total_plan_area;
    double valid_plan_area;
    double coverage_ratio;
    /* Positive existing-design material is cut; negative material is fill. */
    double cut_volume;
    double fill_volume;
    double net_volume;
    double minimum_difference;
    double maximum_difference;
    double mean_difference;
    double rmse_difference;
} dt_grid_earthwork_result;

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

/* Bilinearly samples the containing GRID cell and reports local derivatives.
   Returns DT_E_NOT_FOUND outside the GRID or when any support node is NoData. */
DT_API dt_status DT_CALL dt_grid_analyze_surface_xy(
    dt_grid_handle grid, const dt_point3* query,
    dt_surface_analysis* output_analysis);

/* Derives a full-resolution slope, aspect, or hillshade GRID. The affine
   transform, dimensions and CRS are copied from source_grid. Interior nodes
   use central differences; border nodes use one-sided differences. Nodes
   whose required support contains NoData become NoData in the result. */
DT_API dt_status DT_CALL dt_grid_derive_terrain(
    dt_grid_handle source_grid, const dt_grid_terrain_options* options,
    dt_grid_handle* output_grid);

/* Integrates the piecewise-linear elevation difference of two node-aligned
   GRID surfaces. Dimensions, affine transform and CRS must match. Crossing
   the zero-difference line is clipped analytically, so cut/fill volumes do
   not depend on a sampling budget. output_difference_grid may be null unless
   DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID is requested. */
DT_API dt_status DT_CALL dt_grid_compare_earthwork(
    dt_grid_handle existing_grid, dt_grid_handle design_grid,
    const dt_grid_earthwork_options* options,
    dt_grid_earthwork_result* output_result,
    dt_grid_handle* output_difference_grid);

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
