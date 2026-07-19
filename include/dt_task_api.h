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
    DT_TASK_RESULT_EARTHWORK = 3,
    DT_TASK_RESULT_GRID_OVERVIEW = 4,
    DT_TASK_RESULT_GRID_VERIFICATION = 5,
    DT_TASK_RESULT_GRID_VIEW = 6
};

enum dt_grid_view_request_flags {
    /* Aggregate bins become NoData if any contributing source node is NoData. */
    DT_GRID_VIEW_REQUEST_STRICT_NODATA = 1u << 0,
    /* Allows average display requests to use persisted DGRIDB pyramid levels. */
    DT_GRID_VIEW_REQUEST_USE_PYRAMID = 1u << 1,
    /* Issues a best-effort operating-system prefetch before reading. */
    DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE = 1u << 2,
    /* Verifies and caches intersecting DGRIDB raw-value blocks first. */
    DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS = 1u << 3
};

enum dt_grid_view_result_flags {
    DT_GRID_VIEW_RESULT_PREFETCH_REQUESTED = 1u << 0,
    DT_GRID_VIEW_RESULT_SOURCE_VERIFIED = 1u << 1,
    DT_GRID_VIEW_RESULT_USED_TILE_CACHE = 1u << 2,
    DT_GRID_VIEW_RESULT_CACHE_HIT = 1u << 3,
    DT_GRID_VIEW_RESULT_CACHE_COALESCED = 1u << 4,
    DT_GRID_VIEW_RESULT_DISK_CACHE_HIT = 1u << 5
};

enum dt_grid_view_cache_flags {
    /* Reserved for future cache policies. Must currently be zero. */
    DT_GRID_VIEW_CACHE_DEFAULT = 0
};

enum dt_grid_view_disk_cache_flags {
    DT_GRID_VIEW_DISK_CACHE_READ_ONLY = 1u << 0,
    /* Recreates a package whose source fingerprint or tile geometry differs. */
    DT_GRID_VIEW_DISK_CACHE_RESET_STALE = 1u << 1,
    /* Recreates a package with an invalid header or directory. */
    DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED = 1u << 2
};

enum dt_grid_view_disk_cache_statistics_flags {
    DT_GRID_VIEW_DISK_CACHE_ACTIVE = 1u << 0,
    DT_GRID_VIEW_DISK_CACHE_READ_ONLY_ACTIVE = 1u << 1,
    DT_GRID_VIEW_DISK_CACHE_WRITE_DISABLED = 1u << 2
};

enum dt_grid_view_cache_compact_flags {
    DT_GRID_VIEW_COMPACT_SHRANK = 1u << 0,
    DT_GRID_VIEW_COMPACT_DROPPED_CORRUPTION = 1u << 1
};

enum dt_grid_progressive_view_flags {
    /* Reserved for future scheduling policies. Must currently be zero. */
    DT_GRID_PROGRESSIVE_VIEW_DEFAULT = 0
};

enum dt_grid_progressive_frame_flags {
    DT_GRID_PROGRESSIVE_FRAME_FIRST = 1u << 0,
    DT_GRID_PROGRESSIVE_FRAME_FINAL = 1u << 1
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

/* Borrowed view of an asynchronously generated overview. values remain valid
   until dt_task_destroy(task). row_stride is measured in doubles. */
typedef struct dt_grid_overview_view {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t width;
    uint64_t height;
    uint64_t row_stride;
    const double* values;
    dt_grid_overview_result result;
    uint64_t reserved[2];
} dt_grid_overview_view;

/* One cancellable world-viewport request. The worker maps world_bounds to a
   source window, optionally prefetches and verifies it, then produces an LOD
   image. output dimensions must be positive and no larger than the selected
   source window for aggregate methods. */
typedef struct dt_grid_view_request_options {
    uint32_t struct_size;
    uint32_t flags;
    dt_bounds2 world_bounds;
    uint64_t output_width;
    uint64_t output_height;
    uint32_t padding_nodes;
    int32_t overview_method;
    uint32_t worker_count;
    uint32_t tile_row_count;
    uint64_t reserved[3];
} dt_grid_view_request_options;

/* Borrowed completed viewport result. values remain valid until
   dt_task_destroy(task). verification is populated only when
   DT_GRID_VIEW_RESULT_SOURCE_VERIFIED is present. */
typedef struct dt_grid_view_result {
    uint32_t struct_size;
    uint32_t flags;
    dt_grid_window source_window;
    uint64_t width;
    uint64_t height;
    uint64_t row_stride;
    const double* values;
    dt_grid_overview_result overview;
    dt_grid_verify_result verification;
    /* One for the exact v0.33 path; power-of-two source-node spacing for a
       cached display LOD. */
    uint64_t lod_scale;
    uint64_t tile_count;
    /* Ready-cache hits plus joins of already queued/loading tiles. */
    uint64_t reused_tile_count;
} dt_grid_view_result;

/* Cross-LOD publication policy. initial_lod_multiplier must be a power of two.
   Zero selects 4. maximum_frame_count zero selects 3 and is capped at 8. The
   exact target LOD is always the final frame. */
typedef struct dt_grid_progressive_view_options {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t initial_lod_multiplier;
    uint32_t maximum_frame_count;
    uint64_t reserved[6];
} dt_grid_progressive_view_options;

/* Borrowed immutable frame. A successful getter keeps values valid until
   dt_task_destroy(task), including while finer frames are being published. */
typedef struct dt_grid_progressive_view_frame {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    uint64_t published_frame_count;
    dt_grid_view_result view;
    uint64_t reserved[4];
} dt_grid_progressive_view_frame;

typedef struct dt_grid_view_cache_options {
    uint32_t struct_size;
    uint32_t flags;
    /* Output samples per spatial tile. Zero selects 128. */
    uint32_t tile_width;
    uint32_t tile_height;
    /* Tile producer threads. Zero selects an automatic count, capped at 8. */
    uint32_t worker_count;
    uint32_t reserved0;
    /* Zero selects 128 MiB. The cache may temporarily exceed this by tiles
       still borrowed by active requests. */
    uint64_t maximum_bytes;
    /* Zero selects 4096 directory entries. */
    uint64_t maximum_tiles;
    uint64_t reserved[3];
} dt_grid_view_cache_options;

typedef struct dt_grid_view_cache_statistics {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capacity_bytes;
    uint64_t cached_bytes;
    uint64_t cached_tile_count;
    uint64_t in_flight_tile_count;
    uint64_t request_count;
    uint64_t hit_tile_count;
    uint64_t miss_tile_count;
    uint64_t coalesced_tile_count;
    uint64_t eviction_count;
    uint64_t reserved[2];
} dt_grid_view_cache_statistics;

/* Optional sparse DGTILE sidecar. source_revision is application-defined and
   participates in the source fingerprint; zero is valid. The package also
   fingerprints GRID geometry, CRS, NoData and deterministic source samples. */
typedef struct dt_grid_view_disk_cache_options {
    uint32_t struct_size;
    uint32_t flags;
    const char* utf8_file_name;
    uint64_t source_revision;
    /* Zero selects 2 GiB. Once full, display continues using memory/source. */
    uint64_t maximum_file_bytes;
    uint64_t reserved[4];
} dt_grid_view_disk_cache_options;

typedef struct dt_grid_view_disk_cache_statistics {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capacity_bytes;
    uint64_t file_bytes;
    uint64_t indexed_tile_count;
    uint64_t disk_hit_tile_count;
    uint64_t written_tile_count;
    uint64_t skipped_write_count;
    uint64_t source_fingerprint;
    /* Includes superseded records; indexed_tile_count counts latest keys. */
    uint64_t stored_record_count;
    /* Space occupied by superseded records. Lazy corruption is only known
       after load or compact verification and is not included here. */
    uint64_t reclaimable_bytes;
    uint64_t reserved[2];
} dt_grid_view_disk_cache_statistics;

/* Result of rewriting a writable DGTILE package with only its latest valid
   tile records. A corrupt lazy payload is omitted because it can be rebuilt
   from the source GRID on demand. */
typedef struct dt_grid_view_cache_compact_result {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t input_file_bytes;
    uint64_t output_file_bytes;
    uint64_t reclaimed_bytes;
    uint64_t input_record_count;
    uint64_t output_record_count;
    uint64_t retained_tile_count;
    uint64_t dropped_duplicate_record_count;
    uint64_t dropped_corrupt_tile_count;
    uint64_t reserved[3];
} dt_grid_view_cache_compact_result;

typedef struct dt_grid_view_cache_t* dt_grid_view_cache_handle;

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
/* Copies options and retains the source GRID for the worker lifetime. The
   result buffer is owned by the task and retrieved with
   dt_task_get_grid_overview_result(). */
DT_API dt_status DT_CALL dt_grid_read_overview_async(
    dt_grid_handle grid, const dt_grid_overview_options* options,
    uint64_t output_width, uint64_t output_height,
    dt_task_handle* output_task);
/* Retains the source GRID and verifies only blocks intersecting the window. */
DT_API dt_status DT_CALL dt_grid_verify_window_async(
    dt_grid_handle grid, uint64_t column, uint64_t row,
    uint64_t width, uint64_t height, dt_task_handle* output_task);
/* Combines world-window mapping, prefetch, optional integrity verification,
   and LOD generation in one cancellable worker task. */
DT_API dt_status DT_CALL dt_grid_read_view_async(
    dt_grid_handle grid, const dt_grid_view_request_options* options,
    dt_task_handle* output_task);
/* Creates a reusable 2-D spatial tile directory retaining the source GRID. */
DT_API dt_status DT_CALL dt_grid_view_cache_create(
    dt_grid_handle grid, const dt_grid_view_cache_options* options,
    dt_grid_view_cache_handle* output_cache);
/* Creates a memory cache backed by a sparse, lazily read DGTILE package. */
DT_API dt_status DT_CALL dt_grid_view_cache_create_persistent(
    dt_grid_handle grid, const dt_grid_view_cache_options* memory_options,
    const dt_grid_view_disk_cache_options* disk_options,
    dt_grid_view_cache_handle* output_cache);
/* Submits a display-oriented tiled LOD request. Tile producers are scheduled
   from the viewport center outward and are shared by concurrent requests. */
DT_API dt_status DT_CALL dt_grid_read_view_cached_async(
    dt_grid_view_cache_handle cache,
    const dt_grid_view_request_options* options,
    dt_task_handle* output_task);
/* Publishes immutable coarse-to-fine frames on one task. Existing task APIs
   remain valid; dt_task_get_grid_view_result() returns the final frame after
   successful completion. */
DT_API dt_status DT_CALL dt_grid_read_view_progressive_async(
    dt_grid_view_cache_handle cache,
    const dt_grid_view_request_options* request_options,
    const dt_grid_progressive_view_options* progressive_options,
    dt_task_handle* output_task);
DT_API dt_status DT_CALL dt_grid_view_cache_get_statistics(
    dt_grid_view_cache_handle cache,
    dt_grid_view_cache_statistics* output_statistics);
DT_API dt_status DT_CALL dt_grid_view_cache_get_disk_statistics(
    dt_grid_view_cache_handle cache,
    dt_grid_view_disk_cache_statistics* output_statistics);
DT_API dt_status DT_CALL dt_grid_view_cache_compact(
    dt_grid_view_cache_handle cache,
    dt_grid_view_cache_compact_result* output_result);
DT_API dt_status DT_CALL dt_grid_view_cache_clear(
    dt_grid_view_cache_handle cache);
DT_API void DT_CALL dt_grid_view_cache_destroy(
    dt_grid_view_cache_handle cache);

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
DT_API dt_status DT_CALL dt_task_get_grid_overview_result(
    dt_task_handle task, dt_grid_overview_view* output_view);
DT_API dt_status DT_CALL dt_task_get_grid_verification_result(
    dt_task_handle task, dt_grid_verify_result* output_result);
DT_API dt_status DT_CALL dt_task_get_grid_view_result(
    dt_task_handle task, dt_grid_view_result* output_result);
/* Returns the oldest published frame whose sequence is greater than
   after_sequence. DT_E_NOT_FOUND means no such frame is currently available. */
DT_API dt_status DT_CALL dt_task_get_grid_view_frame(
    dt_task_handle task, uint64_t after_sequence,
    dt_grid_progressive_view_frame* output_frame);
/* Waits until a newer progressive frame is available or the task reaches a
   terminal state. UINT32_MAX waits indefinitely. */
DT_API dt_status DT_CALL dt_task_wait_for_grid_view_frame(
    dt_task_handle task, uint64_t after_sequence,
    uint32_t timeout_milliseconds, int32_t* frame_available);
DT_API dt_status DT_CALL dt_task_get_error(
    dt_task_handle task, char* buffer, size_t buffer_size,
    size_t* required_size);

/* Requests cancellation and waits for the worker before returning. */
DT_API void DT_CALL dt_task_destroy(dt_task_handle task);

#ifdef __cplusplus
}
#endif

#endif
