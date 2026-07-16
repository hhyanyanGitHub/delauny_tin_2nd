#ifndef DT_CDT_API_H
#define DT_CDT_API_H

#include "dt_terrain_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t dt_constraint_id;

enum dt_constraint_kind {
    DT_CONSTRAINT_BREAKLINE = 1,
    DT_CONSTRAINT_OUTER_BOUNDARY = 2,
    DT_CONSTRAINT_HOLE_BOUNDARY = 3
};

enum dt_constraint_flags {
    DT_CONSTRAINT_CLOSED = 1u << 0
};

typedef struct dt_cdt_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t reserved[6];
} dt_cdt_options;

typedef struct dt_cdt_statistics {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t base_point_count;
    uint64_t vertex_count;
    uint64_t finite_triangle_count;
    uint64_t domain_triangle_count;
    uint64_t constraint_count;
    uint64_t constrained_edge_count;
    uint64_t generation;
    dt_bounds2 bounds;
} dt_cdt_statistics;

typedef struct dt_constraint_info {
    uint32_t struct_size;
    uint32_t flags;
    dt_constraint_id id;
    int32_t kind;
    uint32_t reserved;
    uint64_t point_count;
} dt_constraint_info;

typedef struct dt_cdt_query_result_view {
    uint32_t struct_size;
    uint32_t reserved;
    const dt_triangle3* triangles;
    uint64_t triangle_count;
    uint64_t generation;
} dt_cdt_query_result_view;

typedef struct dt_cdt_t* dt_cdt_handle;
typedef struct dt_cdt_query_result_t* dt_cdt_query_result;

DT_API dt_status DT_CALL dt_cdt_create(
    const dt_cdt_options* options, dt_cdt_handle* output_cdt);
DT_API void DT_CALL dt_cdt_destroy(dt_cdt_handle cdt);
DT_API dt_status DT_CALL dt_cdt_clear(dt_cdt_handle cdt);

/* Replaces the unconstrained terrain points atomically and removes constraints. */
DT_API dt_status DT_CALL dt_cdt_build(
    dt_cdt_handle cdt, const dt_point3* points, uint64_t point_count);
/* Copies the current ordinary TIN vertices and CRS into cdt atomically. */
DT_API dt_status DT_CALL dt_cdt_build_from_tin(
    dt_cdt_handle cdt, dt_handle tin);

/* Boundary and hole constraints are always closed and require at least three
   distinct points. Breaklines may be open or DT_CONSTRAINT_CLOSED. */
DT_API dt_status DT_CALL dt_cdt_add_constraint(
    dt_cdt_handle cdt, int32_t kind, uint32_t flags,
    const dt_point3* points, uint64_t point_count,
    dt_constraint_id* output_constraint_id);
/* Atomically replaces one constraint's geometry while preserving its stable
   id and kind. output_effect is optional; requesting it performs a complete
   before/after domain diff and returns a normal dt_edit_result handle. */
DT_API dt_status DT_CALL dt_cdt_update_constraint(
    dt_cdt_handle cdt, dt_constraint_id constraint_id, uint32_t flags,
    const dt_point3* points, uint64_t point_count,
    dt_edit_result* output_effect);
DT_API dt_status DT_CALL dt_cdt_remove_constraint(
    dt_cdt_handle cdt, dt_constraint_id constraint_id);

DT_API dt_status DT_CALL dt_cdt_get_statistics(
    dt_cdt_handle cdt, dt_cdt_statistics* output_statistics);
/* constraint_index is in [0, constraint_count). */
DT_API dt_status DT_CALL dt_cdt_get_constraint_info(
    dt_cdt_handle cdt, uint64_t constraint_index,
    dt_constraint_info* output_info);
/* Reports required_count even when output_points is NULL. */
DT_API dt_status DT_CALL dt_cdt_copy_constraint_points(
    dt_cdt_handle cdt, dt_constraint_id constraint_id,
    dt_point3* output_points, uint64_t point_capacity,
    uint64_t* required_count);

/* Returns triangles inside outer boundaries and outside holes. When no outer
   boundary exists, all finite CDT triangles are treated as domain triangles. */
DT_API dt_status DT_CALL dt_cdt_query_triangles(
    dt_cdt_handle cdt, const dt_bounds2* bounds,
    dt_cdt_query_result* output_result);
DT_API dt_status DT_CALL dt_cdt_query_result_get_view(
    dt_cdt_query_result result, dt_cdt_query_result_view* output_view);
DT_API void DT_CALL dt_cdt_release_query_result(dt_cdt_query_result result);

/* Samples the piecewise-linear CDT surface. Points outside the active domain
   or inside holes return DT_E_NOT_FOUND. Query Z is ignored. */
DT_API dt_status DT_CALL dt_cdt_sample_height_xy(
    dt_cdt_handle cdt, const dt_point3* query, double* output_z);

/* Derived products honor the active CDT domain. GRID nodes outside the domain
   become NoData; contours stop at outer and hole boundaries. */
DT_API dt_status DT_CALL dt_grid_from_cdt(
    dt_cdt_handle cdt, const dt_tin_to_grid_options* options,
    dt_grid_handle* output_grid);
DT_API dt_status DT_CALL dt_contours_from_cdt(
    dt_cdt_handle cdt, const dt_contour_options* options,
    dt_contour_handle* output_contours);

DT_API dt_status DT_CALL dt_cdt_validate(dt_cdt_handle cdt, int32_t verbose);
DT_API dt_status DT_CALL dt_cdt_set_crs_wkt(
    dt_cdt_handle cdt, const char* utf8_crs_wkt);
DT_API dt_status DT_CALL dt_cdt_get_crs_wkt(
    dt_cdt_handle cdt, char* buffer, size_t buffer_size,
    size_t* required_size);

/* DCDT 1 is a portable UTF-8 text format containing points and constraints. */
DT_API dt_status DT_CALL dt_cdt_save_text(
    dt_cdt_handle cdt, const char* utf8_file_name);
DT_API dt_status DT_CALL dt_cdt_load_text(
    dt_cdt_handle cdt, const char* utf8_file_name, dt_bounds2* output_bounds);

#ifdef __cplusplus
}
#endif

#endif
