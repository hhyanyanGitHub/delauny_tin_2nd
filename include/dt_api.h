#ifndef DT_API_H
#define DT_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(DT_BUILD_DLL)
#    define DT_API __declspec(dllexport)
#  else
#    define DT_API __declspec(dllimport)
#  endif
#  define DT_CALL __cdecl
#else
#  define DT_API __attribute__((visibility("default")))
#  define DT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DT_VERSION_MAJOR 0
#define DT_VERSION_MINOR 24
#define DT_VERSION_PATCH 0

typedef int32_t dt_status;
typedef uint64_t dt_vertex_id;

enum dt_status_code {
    DT_OK = 0,
    DT_E_INVALID_ARGUMENT = 1,
    DT_E_NOT_INITIALIZED = 2,
    DT_E_DUPLICATE_XY = 3,
    DT_E_EMPTY = 4,
    DT_E_NOT_FOUND = 5,
    DT_E_IO = 6,
    DT_E_OUT_OF_MEMORY = 7,
    DT_E_CORRUPTED_DATA = 8,
    DT_E_STALE_QUERY = 9,
    DT_E_INTERNAL = 10,
    DT_E_UNSUPPORTED = 11,
    DT_E_CANCELLED = 12,
    DT_E_LIMIT_EXCEEDED = 13
};

enum dt_location_type {
    DT_LOCATION_EMPTY = 0,
    DT_LOCATION_FACE = 1,
    DT_LOCATION_EDGE = 2,
    DT_LOCATION_VERTEX = 3,
    DT_LOCATION_OUTSIDE_HULL = 4,
    DT_LOCATION_OUTSIDE_AFFINE_HULL = 5
};

typedef struct dt_point3 {
    double x;
    double y;
    double z;
} dt_point3;

typedef struct dt_bounds2 {
    double xmin;
    double ymin;
    double xmax;
    double ymax;
} dt_bounds2;

typedef struct dt_vertex3 {
    dt_point3 point;
    dt_vertex_id id;
} dt_vertex3;

typedef struct dt_segment3 {
    dt_vertex3 vertex[2];
} dt_segment3;

typedef struct dt_triangle3 {
    dt_vertex3 vertex[3];
} dt_triangle3;

typedef struct dt_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t reserved[6];
} dt_options;

typedef struct dt_statistics {
    uint32_t struct_size;
    int32_t dimension;
    uint64_t vertex_count;
    uint64_t finite_triangle_count;
    uint64_t generation;
    dt_bounds2 bounds;
} dt_statistics;

typedef struct dt_location_result {
    uint32_t struct_size;
    int32_t type;
    dt_vertex3 vertex;
    dt_segment3 edge;
    dt_triangle3 triangle;
} dt_location_result;

enum dt_surface_analysis_flags {
    /* A horizontal surface has no unique downslope azimuth. */
    DT_SURFACE_ASPECT_UNDEFINED = 1u << 0,
    /* The query lies exactly on a TIN/CDT edge or vertex. One adjacent
       finite active face is selected deterministically as support. */
    DT_SURFACE_QUERY_ON_EDGE = 1u << 1,
    DT_SURFACE_QUERY_ON_VERTEX = 1u << 2,
    /* GRID derivatives come from the local bilinear cell, not one plane. */
    DT_SURFACE_BILINEAR = 1u << 3
};

typedef struct dt_surface_analysis {
    uint32_t struct_size;
    uint32_t flags;
    dt_point3 point;
    double dz_dx;
    double dz_dy;
    double slope_degrees;
    /* Downslope azimuth clockwise from +Y (north), in [0,360). Zero when
       DT_SURFACE_ASPECT_UNDEFINED is set. */
    double aspect_degrees;
    double normal_x;
    double normal_y;
    double normal_z;
    dt_point3 support_points[4];
    uint32_t support_point_count;
    uint32_t reserved;
} dt_surface_analysis;

typedef struct dt_edit_result_view {
    uint32_t struct_size;
    uint32_t reserved;
    const dt_triangle3* removed_triangles;
    uint64_t removed_triangle_count;
    const dt_triangle3* added_triangles;
    uint64_t added_triangle_count;
    const dt_segment3* boundary_edges;
    uint64_t boundary_edge_count;
    const dt_segment3* removed_edges;
    uint64_t removed_edge_count;
    const dt_segment3* added_edges;
    uint64_t added_edge_count;
    dt_vertex_id affected_vertex_id;
    uint64_t generation;
} dt_edit_result_view;

typedef struct dt_query_result_view {
    uint32_t struct_size;
    uint32_t reserved;
    const dt_triangle3* triangles;
    uint64_t triangle_count;
    uint64_t generation;
} dt_query_result_view;

typedef struct dt_context_t* dt_handle;
typedef struct dt_edit_result_t* dt_edit_result;
typedef struct dt_query_result_t* dt_query_result;

DT_API void DT_CALL dt_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch);

DT_API dt_status DT_CALL dt_create(const dt_options* options, dt_handle* out_handle);
DT_API void DT_CALL dt_destroy(dt_handle handle);
DT_API dt_status DT_CALL dt_clear_handle(dt_handle handle);

/* Replaces the current mesh atomically. output_ids may be NULL. */
DT_API dt_status DT_CALL dt_build(dt_handle handle,
                                  const dt_point3* points,
                                  uint64_t point_count,
                                  dt_vertex_id* output_ids);

DT_API dt_status DT_CALL dt_insert_point(dt_handle handle,
                                         const dt_point3* point,
                                         dt_vertex_id* output_id,
                                         dt_edit_result* output_effect);
DT_API dt_status DT_CALL dt_delete_nearest_xy(dt_handle handle,
                                              const dt_point3* query,
                                              dt_vertex_id* deleted_id,
                                              dt_edit_result* output_effect);
DT_API dt_status DT_CALL dt_delete_vertex(dt_handle handle,
                                          dt_vertex_id vertex_id,
                                          dt_edit_result* output_effect);
DT_API dt_status DT_CALL dt_update_vertex_z(dt_handle handle,
                                            dt_vertex_id vertex_id,
                                            double z);

DT_API dt_status DT_CALL dt_find_nearest_vertex_xy(dt_handle handle,
                                                   const dt_point3* query,
                                                   dt_vertex3* output_vertex);
DT_API dt_status DT_CALL dt_locate_point_xy(dt_handle handle,
                                            const dt_point3* query,
                                            dt_location_result* output_result);
DT_API dt_status DT_CALL dt_analyze_tin_surface_xy(
    dt_handle handle, const dt_point3* query,
    dt_surface_analysis* output_analysis);
DT_API dt_status DT_CALL dt_query_triangles(dt_handle handle,
                                            const dt_bounds2* bounds,
                                            dt_query_result* output_result);

DT_API dt_status DT_CALL dt_get_statistics(dt_handle handle,
                                           dt_statistics* output_statistics);
DT_API dt_status DT_CALL dt_validate(dt_handle handle, int32_t verbose);

DT_API dt_status DT_CALL dt_save(dt_handle handle, const char* utf8_file_name);
DT_API dt_status DT_CALL dt_load(dt_handle handle, const char* utf8_file_name,
                                 dt_bounds2* output_bounds);

/*
 * Text I/O:
 * - point text: one X Y Z point per line; whitespace, comma and semicolon
 *   separators are accepted. Blank lines and lines beginning with # are ignored.
 * - mesh text: DTMESH 1 format containing VERTICES and TRIANGLES sections.
 * All loads replace the current mesh atomically and output_bounds may be NULL.
 */
DT_API dt_status DT_CALL dt_import_points_text(dt_handle handle,
                                               const char* utf8_file_name,
                                               dt_bounds2* output_bounds);
DT_API dt_status DT_CALL dt_save_mesh_text(dt_handle handle,
                                           const char* utf8_file_name);
DT_API dt_status DT_CALL dt_load_mesh_text(dt_handle handle,
                                           const char* utf8_file_name,
                                           dt_bounds2* output_bounds);

DT_API dt_status DT_CALL dt_edit_result_get_view(dt_edit_result result,
                                                 dt_edit_result_view* output_view);
DT_API void DT_CALL dt_release_edit_result(dt_edit_result result);
DT_API dt_status DT_CALL dt_query_result_get_view(dt_query_result result,
                                                  dt_query_result_view* output_view);
DT_API void DT_CALL dt_release_query_result(dt_query_result result);

/* CRS is optional UTF-8 OGC WKT metadata. The getter reports the required
   byte count including the trailing NUL. */
DT_API dt_status DT_CALL dt_set_crs_wkt(dt_handle handle,
                                        const char* utf8_crs_wkt);
DT_API dt_status DT_CALL dt_get_crs_wkt(dt_handle handle, char* buffer,
                                        size_t buffer_size,
                                        size_t* required_size);

/* Returns the required byte count including the trailing NUL in required_size. */
DT_API dt_status DT_CALL dt_get_last_error(char* buffer, size_t buffer_size,
                                           size_t* required_size);

#ifdef __cplusplus
}
#endif

#endif
