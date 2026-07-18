#ifndef DT_TASK_API_H
#define DT_TASK_API_H

#include "dt_terrain_api.h"

#ifdef __cplusplus
extern "C" {
#endif

enum dt_task_state {
    DT_TASK_PENDING = 0,
    DT_TASK_RUNNING = 1,
    DT_TASK_SUCCEEDED = 2,
    DT_TASK_FAILED = 3,
    DT_TASK_CANCELLED = 4
};

enum dt_task_result_kind {
    DT_TASK_RESULT_NONE = 0,
    DT_TASK_RESULT_GRID = 1,
    DT_TASK_RESULT_CONTOURS = 2,
    DT_TASK_RESULT_EARTHWORK = 3
};

typedef struct dt_task_info {
    uint32_t struct_size;
    int32_t state;
    int32_t result_kind;
    dt_status result_status;
    double progress;
    int32_t cancellation_requested;
    uint32_t reserved;
} dt_task_info;

typedef struct dt_task_t* dt_task_handle;

DT_API dt_status DT_CALL dt_grid_from_tin_async(
    dt_handle tin, const dt_tin_to_grid_options* options,
    dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_grid_derive_terrain_async(
    dt_grid_handle source_grid, const dt_grid_terrain_options* options,
    dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_grid_compare_earthwork_async(
    dt_grid_handle existing_grid, dt_grid_handle design_grid,
    const dt_grid_earthwork_options* options, dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_grid_resample_like_async(
    dt_grid_handle source_grid, dt_grid_handle reference_grid,
    const dt_grid_resample_options* options, dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_grid_clip_polygon_async(
    dt_grid_handle source_grid, const dt_point3* polygon_points,
    uint64_t point_count, const dt_grid_clip_options* options,
    dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_tin_from_grid_async(
    dt_grid_handle grid, const dt_grid_to_tin_options* options,
    dt_handle output_tin, dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_contours_from_tin_async(
    dt_handle tin, const dt_contour_options* options,
    dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_contours_from_grid_async(
    dt_grid_handle grid, const dt_contour_options* options,
    dt_task_handle* output_task);

DT_API dt_status DT_CALL dt_task_get_info(
    dt_task_handle task, dt_task_info* output_info);
/* UINT32_MAX waits indefinitely. completed is zero on timeout. */
DT_API dt_status DT_CALL dt_task_wait(
    dt_task_handle task, uint32_t timeout_milliseconds, int32_t* completed);
DT_API dt_status DT_CALL dt_task_request_cancel(dt_task_handle task);

/* Result handles share immutable completed data and are owned by the caller. */
DT_API dt_status DT_CALL dt_task_get_grid_result(
    dt_task_handle task, dt_grid_handle* output_grid);
DT_API dt_status DT_CALL dt_task_get_contour_result(
    dt_task_handle task, dt_contour_handle* output_contours);
/* output_difference_grid is optional. It is non-null only when requested in
   the earthwork options and is owned by the caller. */
DT_API dt_status DT_CALL dt_task_get_earthwork_result(
    dt_task_handle task, dt_grid_earthwork_result* output_result,
    dt_grid_handle* output_difference_grid);
DT_API dt_status DT_CALL dt_task_get_error(
    dt_task_handle task, char* buffer, size_t buffer_size,
    size_t* required_size);

/* Requests cancellation and waits for the worker before returning. */
DT_API void DT_CALL dt_task_destroy(dt_task_handle task);

#ifdef __cplusplus
}
#endif

#endif
