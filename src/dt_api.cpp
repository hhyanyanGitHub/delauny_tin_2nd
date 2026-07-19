#include "dt_core.hpp"
#include "dt_cdt_api.h"
#include "dt_cdt_core.hpp"
#include "dt_gdal_api.h"
#include "dt_task_api.h"
#include "dt_terrain_core.hpp"
#include "dt_view_cache.hpp"
#if DT_WITH_GDAL
#include "dt_gdal_io.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct dt_context_t {
    std::shared_ptr<dt::Context> context = std::make_shared<dt::Context>();
};

struct dt_edit_result_t {
    dt::EditData data;
};

struct dt_query_result_t {
    dt::QueryData data;
};

struct dt_grid_t {
    std::shared_ptr<dt::Grid> grid;
};

struct dt_grid_view_cache_t {
    std::shared_ptr<dt::GridViewCache> cache;
};

struct dt_contour_set_t {
    std::shared_ptr<dt::ContourSet> contours;
};

struct dt_cdt_t {
    std::shared_ptr<dt::CdtContext> context =
        std::make_shared<dt::CdtContext>();
};

struct dt_cdt_query_result_t {
    dt::CdtQueryData data;
};

struct dt_task_t {
    std::mutex mutex;
    std::condition_variable completed;
    std::thread worker;
    std::atomic<double> progress{0.0};
    std::atomic<bool> cancellation_requested{false};
    int32_t state = DT_TASK_PENDING;
    int32_t result_kind = DT_TASK_RESULT_NONE;
    dt_status result_status = DT_OK;
    std::string error;
    std::shared_ptr<dt::Grid> grid_result;
    std::shared_ptr<dt::ContourSet> contour_result;
    dt_grid_earthwork_result earthwork_result{};
    bool earthwork_result_ready = false;
    std::vector<double> overview_values;
    dt_grid_overview_result overview_result{};
    uint64_t overview_width = 0;
    uint64_t overview_height = 0;
    bool overview_result_ready = false;
    dt_grid_verify_result verification_result{};
    bool verification_result_ready = false;
    dt_grid_window view_window{};
    uint32_t view_result_flags = 0;
    uint64_t view_lod_scale = 1;
    uint64_t view_tile_count = 0;
    uint64_t view_reused_tile_count = 0;
    bool view_result_ready = false;
};

namespace {

thread_local std::string* g_last_error = nullptr;

std::string& last_error() {
    /* Deliberately retained until process exit. This avoids a GCC 14 MinGW
     * __cxa_thread_atexit ABI conflict for non-trivial TLS objects. */
    if (!g_last_error) g_last_error = new std::string();
    return *g_last_error;
}

void set_error(const char* message) {
    last_error() = message ? message : "unknown error";
}

template <class Function>
dt_status guarded(Function&& function) noexcept {
    try {
        last_error().clear();
        function();
        return DT_OK;
    } catch (const dt::Exception& error) {
        set_error(error.what());
        return error.status();
    } catch (const std::bad_alloc&) {
        set_error("memory allocation failed");
        return DT_E_OUT_OF_MEMORY;
    } catch (const std::exception& error) {
        set_error(error.what());
        return DT_E_INTERNAL;
    } catch (...) {
        set_error("unknown internal error");
        return DT_E_INTERNAL;
    }
}

dt::Context& require_context(dt_handle handle) {
    if (!handle || !handle->context) {
        throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid context handle");
    }
    return *handle->context;
}

dt::CdtContext& require_cdt(dt_cdt_handle handle) {
    if (!handle || !handle->context) {
        throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid CDT handle");
    }
    return *handle->context;
}

dt_task_t& require_task(dt_task_handle handle) {
    if (!handle) throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid task handle");
    return *handle;
}

std::shared_ptr<dt::Context> require_context_shared(dt_handle handle) {
    require_context(handle);
    return handle->context;
}

dt::Grid& require_grid(dt_grid_handle handle) {
    if (!handle || !handle->grid) {
        throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid grid handle");
    }
    return *handle->grid;
}

std::shared_ptr<dt::Grid> require_grid_shared(dt_grid_handle handle) {
    require_grid(handle);
    return handle->grid;
}

std::shared_ptr<dt::GridViewCache> require_grid_view_cache_shared(
    dt_grid_view_cache_handle handle) {
    if (!handle || !handle->cache) {
        throw dt::Exception(DT_E_NOT_INITIALIZED,
                            "invalid GRID view cache handle");
    }
    return handle->cache;
}

dt::ContourSet& require_contours(dt_contour_handle handle) {
    if (!handle || !handle->contours) {
        throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid contour handle");
    }
    return *handle->contours;
}

template <class T>
void validate_options(const T* options, const char* name) {
    if (!options) {
        throw dt::Exception(DT_E_INVALID_ARGUMENT,
                            std::string(name) + " is null");
    }
    if (options->struct_size != 0 && options->struct_size < sizeof(T)) {
        throw dt::Exception(DT_E_INVALID_ARGUMENT,
                            std::string(name) +
                                " has an incompatible struct_size");
    }
}

std::vector<dt_point3> copy_clip_polygon(const dt_point3* points,
                                         uint64_t point_count) {
    constexpr uint64_t kMaximumClipVertices = 10000000ULL;
    if (!points || point_count < 3) {
        throw dt::Exception(DT_E_INVALID_ARGUMENT,
                            "GRID clip polygon needs at least three points");
    }
    if (point_count > kMaximumClipVertices ||
        point_count > static_cast<uint64_t>(
                          std::numeric_limits<size_t>::max())) {
        throw dt::Exception(DT_E_LIMIT_EXCEEDED,
                            "GRID clip polygon has too many points");
    }
    return std::vector<dt_point3>(points, points + point_count);
}

bool task_finished(int32_t state) noexcept {
    return state == DT_TASK_SUCCEEDED || state == DT_TASK_FAILED ||
           state == DT_TASK_CANCELLED;
}

dt_status copy_utf8_string(const std::string& value, char* buffer,
                           size_t buffer_size, size_t* required_size) {
    const size_t required = value.size() + 1;
    if (required_size) *required_size = required;
    if (!buffer) return buffer_size == 0 ? DT_OK : DT_E_INVALID_ARGUMENT;
    if (buffer_size == 0) return DT_E_INVALID_ARGUMENT;
    const size_t copied = std::min(buffer_size - 1, value.size());
    std::memcpy(buffer, value.data(), copied);
    buffer[copied] = '\0';
    return buffer_size >= required ? DT_OK : DT_E_INVALID_ARGUMENT;
}

void finish_task(dt_task_t& task, int32_t state, dt_status status,
                 const char* error = nullptr) {
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.state = state;
        task.result_status = status;
        task.error = error ? error : "";
        if (state == DT_TASK_SUCCEEDED) task.progress.store(1.0);
    }
    task.completed.notify_all();
}

template <class Function>
dt_task_handle start_task(int32_t result_kind, Function&& function) {
    auto task = std::make_unique<dt_task_t>();
    task->result_kind = result_kind;
    dt_task_t* raw = task.get();
    raw->worker = std::thread(
        [raw, function = std::forward<Function>(function)]() mutable {
            {
                std::lock_guard<std::mutex> lock(raw->mutex);
                raw->state = DT_TASK_RUNNING;
            }
            try {
                if (raw->cancellation_requested.load()) {
                    throw dt::Exception(DT_E_CANCELLED,
                                        "terrain operation was cancelled");
                }
                function(*raw);
                if (raw->cancellation_requested.load()) {
                    throw dt::Exception(DT_E_CANCELLED,
                                        "terrain operation was cancelled");
                }
                finish_task(*raw, DT_TASK_SUCCEEDED, DT_OK);
            } catch (const dt::Exception& error) {
                finish_task(*raw,
                            error.status() == DT_E_CANCELLED ? DT_TASK_CANCELLED
                                                            : DT_TASK_FAILED,
                            error.status(), error.what());
            } catch (const std::bad_alloc&) {
                finish_task(*raw, DT_TASK_FAILED, DT_E_OUT_OF_MEMORY,
                            "memory allocation failed");
            } catch (const std::exception& error) {
                finish_task(*raw, DT_TASK_FAILED, DT_E_INTERNAL, error.what());
            } catch (...) {
                finish_task(*raw, DT_TASK_FAILED, DT_E_INTERNAL,
                            "unknown internal error");
            }
        });
    return task.release();
}

} // namespace

extern "C" {

dt_status DT_CALL dt_gdal_initialize(void) {
    return guarded([&] {
#if DT_WITH_GDAL
        dt::gdal_initialize();
#else
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

dt_status DT_CALL dt_gdal_is_driver_available(const char* driver_name,
                                               int32_t* output_available) {
    if (output_available) *output_available = 0;
    return guarded([&] {
        if (!output_available) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_available is null");
        }
#if DT_WITH_GDAL
        *output_available = dt::gdal_driver_available(driver_name) ? 1 : 0;
#else
        (void)driver_name;
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

dt_status DT_CALL dt_grid_load_gdal_raster(
    const char* utf8_file_name, const dt_gdal_raster_load_options* options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        dt_gdal_raster_load_options actual{};
        actual.struct_size = sizeof(actual);
        if (options) {
            validate_options(options, "dt_gdal_raster_load_options");
            actual = *options;
        }
#if DT_WITH_GDAL
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_load_gdal(utf8_file_name, actual);
        *output_grid = result.release();
#else
        (void)utf8_file_name;
        (void)actual;
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

dt_status DT_CALL dt_grid_save_gdal_raster(
    dt_grid_handle grid, const char* utf8_file_name,
    const dt_gdal_raster_save_options* options) {
    return guarded([&] {
        dt_gdal_raster_save_options actual{};
        actual.struct_size = sizeof(actual);
        if (options) {
            validate_options(options, "dt_gdal_raster_save_options");
            actual = *options;
        }
#if DT_WITH_GDAL
        dt::grid_save_gdal(require_grid(grid), utf8_file_name, actual);
#else
        (void)grid;
        (void)utf8_file_name;
        (void)actual;
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

dt_status DT_CALL dt_contours_load_gdal_vector(
    const char* utf8_file_name, const dt_gdal_contour_load_options* options,
    dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        dt_gdal_contour_load_options actual{};
        actual.struct_size = sizeof(actual);
        if (options) {
            validate_options(options, "dt_gdal_contour_load_options");
            actual = *options;
        }
#if DT_WITH_GDAL
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours = dt::contours_load_gdal(utf8_file_name, actual);
        *output_contours = result.release();
#else
        (void)utf8_file_name;
        (void)actual;
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

dt_status DT_CALL dt_contours_save_gdal_vector(
    dt_contour_handle contours, const char* utf8_file_name,
    const dt_gdal_contour_save_options* options) {
    return guarded([&] {
        dt_gdal_contour_save_options actual{};
        actual.struct_size = sizeof(actual);
        if (options) {
            validate_options(options, "dt_gdal_contour_save_options");
            actual = *options;
        }
#if DT_WITH_GDAL
        dt::contours_save_gdal(require_contours(contours), utf8_file_name,
                               actual);
#else
        (void)contours;
        (void)utf8_file_name;
        (void)actual;
        throw dt::Exception(DT_E_UNSUPPORTED,
                            "dterrain was built without GDAL support");
#endif
    });
}

void DT_CALL dt_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major) *major = DT_VERSION_MAJOR;
    if (minor) *minor = DT_VERSION_MINOR;
    if (patch) *patch = DT_VERSION_PATCH;
}

dt_status DT_CALL dt_grid_create(const dt_grid_create_options* options,
                                 dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_create_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = std::make_unique<dt::Grid>(*options);
        *output_grid = result.release();
    });
}

void DT_CALL dt_grid_destroy(dt_grid_handle grid) {
    delete grid;
}

dt_status DT_CALL dt_grid_get_info(dt_grid_handle grid,
                                   dt_grid_info* output_info) {
    return guarded([&] {
        if (!output_info) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_info is null");
        }
        *output_info = require_grid(grid).info();
    });
}

dt_status DT_CALL dt_grid_set_crs_wkt(dt_grid_handle grid,
                                       const char* utf8_crs_wkt) {
    return guarded([&] {
        require_grid(grid).set_crs_wkt(utf8_crs_wkt ? utf8_crs_wkt : "");
    });
}

dt_status DT_CALL dt_grid_get_crs_wkt(dt_grid_handle grid, char* buffer,
                                       size_t buffer_size,
                                       size_t* required_size) {
    return guarded([&] {
        const auto value = require_grid(grid).crs_wkt();
        const dt_status status =
            copy_utf8_string(value, buffer, buffer_size, required_size);
        if (status != DT_OK) {
            throw dt::Exception(status, "CRS output buffer is too small");
        }
    });
}

dt_status DT_CALL dt_grid_read_window(dt_grid_handle grid, uint64_t column,
                                       uint64_t row, uint64_t width,
                                       uint64_t height, double* output_values,
                                       uint64_t row_stride) {
    return guarded([&] {
        require_grid(grid).read_window(column, row, width, height, output_values,
                                       row_stride);
    });
}

dt_status DT_CALL dt_grid_prefetch_window(dt_grid_handle grid,
                                           uint64_t column, uint64_t row,
                                           uint64_t width, uint64_t height) {
    return guarded([&] {
        require_grid(grid).prefetch_window(column, row, width, height);
    });
}

dt_status DT_CALL dt_grid_verify_window(
    dt_grid_handle grid, uint64_t column, uint64_t row,
    uint64_t width, uint64_t height, dt_grid_verify_result* output_result) {
    if (output_result) *output_result = {};
    return guarded([&] {
        if (!output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_result is null");
        }
        *output_result = require_grid(grid).verify_window(
            column, row, width, height);
    });
}

dt_status DT_CALL dt_grid_read_overview(
    dt_grid_handle grid, const dt_grid_overview_options* options,
    uint64_t output_width, uint64_t output_height, double* output_values,
    uint64_t row_stride, dt_grid_overview_result* output_result) {
    if (output_result) *output_result = {};
    return guarded([&] {
        validate_options(options, "dt_grid_overview_options");
        const auto result = require_grid(grid).read_overview(
            *options, output_width, output_height, output_values, row_stride);
        if (output_result) *output_result = result;
    });
}

dt_status DT_CALL dt_grid_get_view_window(
    dt_grid_handle grid, const dt_grid_view_options* options,
    dt_grid_window* output_window) {
    if (output_window) *output_window = {};
    return guarded([&] {
        validate_options(options, "dt_grid_view_options");
        if (!output_window) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_window is null");
        }
        *output_window = require_grid(grid).view_window(*options);
    });
}

dt_status DT_CALL dt_grid_write_window(dt_grid_handle grid, uint64_t column,
                                       uint64_t row, uint64_t width,
                                       uint64_t height, const double* values,
                                       uint64_t row_stride) {
    return guarded([&] {
        require_grid(grid).write_window(column, row, width, height, values,
                                        row_stride);
    });
}

dt_status DT_CALL dt_grid_analyze_surface_xy(
    dt_grid_handle grid, const dt_point3* query,
    dt_surface_analysis* output_analysis) {
    return guarded([&] {
        if (!query || !output_analysis) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_analysis is null");
        }
        *output_analysis = require_grid(grid).analyze_surface_xy(*query);
    });
}

dt_status DT_CALL dt_grid_derive_terrain(
    dt_grid_handle source_grid, const dt_grid_terrain_options* options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_terrain_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_derive_terrain(require_grid(source_grid),
                                               *options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_grid_resample_like(
    dt_grid_handle source_grid, dt_grid_handle reference_grid,
    const dt_grid_resample_options* options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_resample_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_resample_like(
            require_grid(source_grid), require_grid(reference_grid),
            *options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_grid_clip_polygon(
    dt_grid_handle source_grid, const dt_point3* polygon_points,
    uint64_t point_count, const dt_grid_clip_options* options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_clip_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_grid is null");
        }
        auto polygon = copy_clip_polygon(polygon_points, point_count);
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_clip_polygon(
            require_grid(source_grid), polygon, *options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_grid_compare_earthwork(
    dt_grid_handle existing_grid, dt_grid_handle design_grid,
    const dt_grid_earthwork_options* options,
    dt_grid_earthwork_result* output_result,
    dt_grid_handle* output_difference_grid) {
    if (output_result) *output_result = {};
    if (output_difference_grid) *output_difference_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_earthwork_options");
        if (!output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_result is null");
        }
        if ((options->flags &
             DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID) != 0 &&
            !output_difference_grid) {
            throw dt::Exception(
                DT_E_INVALID_ARGUMENT,
                "output_difference_grid is required by earthwork flags");
        }
        auto computation = dt::grid_compare_earthwork(
            require_grid(existing_grid), require_grid(design_grid), *options);
        *output_result = computation.result;
        if (computation.difference_grid) {
            auto result = std::make_unique<dt_grid_t>();
            result->grid = std::move(computation.difference_grid);
            *output_difference_grid = result.release();
        }
    });
}

dt_status DT_CALL dt_grid_save_text(dt_grid_handle grid,
                                    const char* utf8_file_name) {
    return guarded([&] { require_grid(grid).save_text(utf8_file_name); });
}

dt_status DT_CALL dt_grid_load_text(const char* utf8_file_name,
                                    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::Grid::load_text(utf8_file_name);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_grid_save_binary(dt_grid_handle grid,
                                      const char* utf8_file_name) {
    return guarded([&] { require_grid(grid).save_binary(utf8_file_name); });
}

dt_status DT_CALL dt_grid_load_binary(const char* utf8_file_name,
                                      dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::Grid::load_binary(utf8_file_name);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_grid_verify_binary_file(const char* utf8_file_name) {
    return guarded([&] { dt::Grid::verify_binary_file(utf8_file_name); });
}

dt_status DT_CALL dt_grid_from_tin(dt_handle tin,
                                   const dt_tin_to_grid_options* options,
                                   dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_tin_to_grid_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_from_tin(require_context(tin), *options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_tin_from_grid(dt_grid_handle grid,
                                   const dt_grid_to_tin_options* options,
                                   dt_handle output_tin) {
    return guarded([&] {
        validate_options(options, "dt_grid_to_tin_options");
        auto points = dt::points_from_grid(require_grid(grid), *options);
        auto& destination = require_context(output_tin);
        destination.build(points.data(), points.size(), nullptr);
        destination.set_crs_wkt(require_grid(grid).crs_wkt());
    });
}

dt_status DT_CALL dt_tin_from_contours(
    dt_contour_handle contours,
    const dt_contours_to_tin_options* options, dt_handle output_tin) {
    return guarded([&] {
        validate_options(options, "dt_contours_to_tin_options");
        auto points =
            dt::points_from_contours(require_contours(contours), *options);
        auto& destination = require_context(output_tin);
        destination.build(points.data(), points.size(), nullptr);
        destination.set_crs_wkt(require_contours(contours).crs_wkt);
    });
}

dt_status DT_CALL dt_grid_from_contours(
    dt_contour_handle contours,
    const dt_contours_to_tin_options* tin_options,
    const dt_tin_to_grid_options* grid_options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(tin_options, "dt_contours_to_tin_options");
        validate_options(grid_options, "dt_tin_to_grid_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_grid is null");
        }
        auto points = dt::points_from_contours(require_contours(contours),
                                               *tin_options);
        dt::Context sampled_tin;
        sampled_tin.build(points.data(), points.size(), nullptr);
        sampled_tin.set_crs_wkt(require_contours(contours).crs_wkt);
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_from_tin(sampled_tin, *grid_options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_contours_from_tin(dt_handle tin,
                                       const dt_contour_options* options,
                                       dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        validate_options(options, "dt_contour_options");
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours =
            dt::contours_from_tin(require_context(tin), *options);
        *output_contours = result.release();
    });
}

dt_status DT_CALL dt_contours_from_grid(dt_grid_handle grid,
                                        const dt_contour_options* options,
                                        dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        validate_options(options, "dt_contour_options");
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours =
            dt::contours_from_grid(require_grid(grid), *options);
        *output_contours = result.release();
    });
}

void DT_CALL dt_contours_destroy(dt_contour_handle contours) {
    delete contours;
}

dt_status DT_CALL dt_contours_get_info(dt_contour_handle contours,
                                       dt_contour_info* output_info) {
    return guarded([&] {
        if (!output_info) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_info is null");
        }
        *output_info = require_contours(contours).info();
    });
}

dt_status DT_CALL dt_contours_get_line(dt_contour_handle contours,
                                       uint64_t line_index,
                                       dt_contour_line_view* output_line) {
    return guarded([&] {
        if (!output_line) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_line is null");
        }
        auto& lines = require_contours(contours).lines;
        if (line_index >= lines.size()) {
            throw dt::Exception(DT_E_NOT_FOUND,
                                "contour line index is out of range");
        }
        const auto& line = lines[static_cast<size_t>(line_index)];
        dt_contour_line_view view{};
        view.struct_size = sizeof(view);
        view.flags = line.flags;
        view.elevation = line.elevation;
        view.points = line.points.data();
        view.point_count = line.points.size();
        *output_line = view;
    });
}

dt_status DT_CALL dt_contours_set_crs_wkt(dt_contour_handle contours,
                                           const char* utf8_crs_wkt) {
    return guarded([&] {
        require_contours(contours).crs_wkt =
            utf8_crs_wkt ? utf8_crs_wkt : "";
    });
}

dt_status DT_CALL dt_contours_get_crs_wkt(dt_contour_handle contours,
                                           char* buffer, size_t buffer_size,
                                           size_t* required_size) {
    return guarded([&] {
        const auto value = require_contours(contours).crs_wkt;
        const dt_status status =
            copy_utf8_string(value, buffer, buffer_size, required_size);
        if (status != DT_OK) {
            throw dt::Exception(status, "CRS output buffer is too small");
        }
    });
}

dt_status DT_CALL dt_contours_save_text(dt_contour_handle contours,
                                        const char* utf8_file_name) {
    return guarded(
        [&] { require_contours(contours).save_text(utf8_file_name); });
}

dt_status DT_CALL dt_contours_load_text(const char* utf8_file_name,
                                        dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours = dt::ContourSet::load_text(utf8_file_name);
        *output_contours = result.release();
    });
}

dt_status DT_CALL dt_grid_from_tin_async(
    dt_handle tin, const dt_tin_to_grid_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_tin_to_grid_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto context = require_context_shared(tin);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID,
            [context, copied_options](dt_task_t& task) {
                task.grid_result = dt::grid_from_tin(
                    *context, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_grid_derive_terrain_async(
    dt_grid_handle source_grid, const dt_grid_terrain_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_terrain_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(source_grid);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID,
            [source, copied_options](dt_task_t& task) {
                task.grid_result = dt::grid_derive_terrain(
                    *source, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_grid_resample_like_async(
    dt_grid_handle source_grid, dt_grid_handle reference_grid,
    const dt_grid_resample_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_resample_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(source_grid);
        const auto reference = require_grid_shared(reference_grid);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID,
            [source, reference, copied_options](dt_task_t& task) {
                task.grid_result = dt::grid_resample_like(
                    *source, *reference, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_grid_clip_polygon_async(
    dt_grid_handle source_grid, const dt_point3* polygon_points,
    uint64_t point_count, const dt_grid_clip_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_clip_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(source_grid);
        const auto copied_options = *options;
        auto polygon = copy_clip_polygon(polygon_points, point_count);
        *output_task = start_task(
            DT_TASK_RESULT_GRID,
            [source, copied_options, polygon = std::move(polygon)](
                dt_task_t& task) {
                task.grid_result = dt::grid_clip_polygon(
                    *source, polygon, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_grid_compare_earthwork_async(
    dt_grid_handle existing_grid, dt_grid_handle design_grid,
    const dt_grid_earthwork_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_earthwork_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto existing = require_grid_shared(existing_grid);
        const auto design = require_grid_shared(design_grid);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_EARTHWORK,
            [existing, design, copied_options](dt_task_t& task) {
                auto computation = dt::grid_compare_earthwork(
                    *existing, *design, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
                task.earthwork_result = computation.result;
                task.earthwork_result_ready = true;
                task.grid_result = std::move(computation.difference_grid);
            });
    });
}

dt_status DT_CALL dt_tin_from_grid_async(
    dt_grid_handle grid, const dt_grid_to_tin_options* options,
    dt_handle output_tin, dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_to_tin_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(grid);
        const auto destination = require_context_shared(output_tin);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_NONE,
            [source, destination, copied_options](dt_task_t& task) {
                auto points = dt::points_from_grid(
                    *source, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
                if (task.cancellation_requested.load()) {
                    throw dt::Exception(DT_E_CANCELLED,
                                        "terrain operation was cancelled");
                }
                destination->build(points.data(), points.size(), nullptr);
                destination->set_crs_wkt(source->crs_wkt());
                task.progress.store(1.0);
            });
    });
}

dt_status DT_CALL dt_contours_from_tin_async(
    dt_handle tin, const dt_contour_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_contour_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto context = require_context_shared(tin);
        auto copied_options = *options;
        std::vector<double> levels;
        if (options->level_count != 0) {
            if (!options->levels) {
                throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                    "contour levels pointer is null");
            }
            if (options->level_count > 1000000ULL) {
                throw dt::Exception(DT_E_LIMIT_EXCEEDED,
                                    "too many contour levels");
            }
            levels.assign(options->levels,
                          options->levels + options->level_count);
        }
        *output_task = start_task(
            DT_TASK_RESULT_CONTOURS,
            [context, copied_options, levels = std::move(levels)](
                dt_task_t& task) mutable {
                copied_options.levels = levels.empty() ? nullptr : levels.data();
                task.contour_result = dt::contours_from_tin(
                    *context, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_contours_from_grid_async(
    dt_grid_handle grid, const dt_contour_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_contour_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(grid);
        auto copied_options = *options;
        std::vector<double> levels;
        if (options->level_count != 0) {
            if (!options->levels) {
                throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                    "contour levels pointer is null");
            }
            if (options->level_count > 1000000ULL) {
                throw dt::Exception(DT_E_LIMIT_EXCEEDED,
                                    "too many contour levels");
            }
            levels.assign(options->levels,
                          options->levels + options->level_count);
        }
        *output_task = start_task(
            DT_TASK_RESULT_CONTOURS,
            [source, copied_options, levels = std::move(levels)](
                dt_task_t& task) mutable {
                copied_options.levels = levels.empty() ? nullptr : levels.data();
                task.contour_result = dt::contours_from_grid(
                    *source, copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
            });
    });
}

dt_status DT_CALL dt_grid_read_overview_async(
    dt_grid_handle grid, const dt_grid_overview_options* options,
    uint64_t output_width, uint64_t output_height,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_overview_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(grid);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID_OVERVIEW,
            [source, copied_options, output_width, output_height](
                dt_task_t& task) {
                constexpr uint64_t kMaximumOverviewDimension =
                    1024ULL * 1024ULL;
                constexpr uint64_t kMaximumOverviewValues = 1000000000ULL;
                if (output_width == 0 || output_height == 0) {
                    throw dt::Exception(
                        DT_E_INVALID_ARGUMENT,
                        "GRID overview output dimensions must be positive");
                }
                if (output_width > kMaximumOverviewDimension ||
                    output_height > kMaximumOverviewDimension ||
                    output_width > kMaximumOverviewValues / output_height) {
                    throw dt::Exception(
                        DT_E_LIMIT_EXCEEDED,
                        "GRID overview output size is invalid or too large");
                }
                task.overview_width = output_width;
                task.overview_height = output_height;
                task.overview_values.resize(static_cast<size_t>(
                    output_width * output_height));
                task.overview_result = source->read_overview(
                    copied_options, output_width, output_height,
                    task.overview_values.data(), output_width,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
                task.overview_result_ready = true;
            });
    });
}

dt_status DT_CALL dt_grid_verify_window_async(
    dt_grid_handle grid, uint64_t column, uint64_t row,
    uint64_t width, uint64_t height, dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        const auto source = require_grid_shared(grid);
        *output_task = start_task(
            DT_TASK_RESULT_GRID_VERIFICATION,
            [source, column, row, width, height](dt_task_t& task) {
                task.verification_result = source->verify_window(
                    column, row, width, height,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
                task.verification_result_ready = true;
            });
    });
}

dt_status DT_CALL dt_grid_read_view_async(
    dt_grid_handle grid, const dt_grid_view_request_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_view_request_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        constexpr uint32_t kKnownFlags =
            DT_GRID_VIEW_REQUEST_STRICT_NODATA |
            DT_GRID_VIEW_REQUEST_USE_PYRAMID |
            DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE |
            DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS;
        if ((options->flags & ~kKnownFlags) != 0) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "unknown GRID view request flags");
        }
        const auto source = require_grid_shared(grid);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID_VIEW,
            [source, copied_options](dt_task_t& task) {
                constexpr uint64_t kMaximumOverviewDimension =
                    1024ULL * 1024ULL;
                constexpr uint64_t kMaximumOverviewValues = 1000000000ULL;
                if (copied_options.output_width == 0 ||
                    copied_options.output_height == 0) {
                    throw dt::Exception(
                        DT_E_INVALID_ARGUMENT,
                        "GRID view output dimensions must be positive");
                }
                if (copied_options.output_width > kMaximumOverviewDimension ||
                    copied_options.output_height > kMaximumOverviewDimension ||
                    copied_options.output_width >
                        kMaximumOverviewValues / copied_options.output_height) {
                    throw dt::Exception(
                        DT_E_LIMIT_EXCEEDED,
                        "GRID view output size is invalid or too large");
                }

                dt_grid_view_options view{};
                view.struct_size = sizeof(view);
                view.world_bounds = copied_options.world_bounds;
                view.padding_nodes = copied_options.padding_nodes;
                task.progress.store(0.0);
                task.view_window = source->view_window(view);
                const auto cancelled = [&] {
                    return task.cancellation_requested.load();
                };
                if (cancelled()) {
                    throw dt::Exception(DT_E_CANCELLED,
                                        "terrain operation was cancelled");
                }

                if ((copied_options.flags &
                     DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE) != 0) {
                    source->prefetch_window(
                        task.view_window.column, task.view_window.row,
                        task.view_window.width, task.view_window.height);
                    task.view_result_flags |=
                        DT_GRID_VIEW_RESULT_PREFETCH_REQUESTED;
                }

                const bool verify =
                    (copied_options.flags &
                     DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS) != 0;
                const double overview_progress_begin = verify ? 0.3 : 0.0;
                if (verify) {
                    task.verification_result = source->verify_window(
                        task.view_window.column, task.view_window.row,
                        task.view_window.width, task.view_window.height,
                        [&](double value) {
                            task.progress.store(value *
                                                overview_progress_begin);
                        },
                        cancelled);
                    task.view_result_flags |=
                        DT_GRID_VIEW_RESULT_SOURCE_VERIFIED;
                } else {
                    task.verification_result = {};
                    task.verification_result.struct_size =
                        sizeof(task.verification_result);
                }

                dt_grid_overview_options overview{};
                overview.struct_size = sizeof(overview);
                overview.method = copied_options.overview_method;
                overview.worker_count = copied_options.worker_count;
                overview.tile_row_count = copied_options.tile_row_count;
                overview.source_column = task.view_window.column;
                overview.source_row = task.view_window.row;
                overview.source_width = task.view_window.width;
                overview.source_height = task.view_window.height;
                if ((copied_options.flags &
                     DT_GRID_VIEW_REQUEST_STRICT_NODATA) != 0) {
                    overview.flags |= DT_GRID_OVERVIEW_STRICT_NODATA;
                }
                if ((copied_options.flags &
                     DT_GRID_VIEW_REQUEST_USE_PYRAMID) != 0) {
                    overview.flags |= DT_GRID_OVERVIEW_USE_PYRAMID;
                }

                task.overview_width = copied_options.output_width;
                task.overview_height = copied_options.output_height;
                task.overview_values.resize(static_cast<size_t>(
                    copied_options.output_width *
                    copied_options.output_height));
                task.overview_result = source->read_overview(
                    overview, copied_options.output_width,
                    copied_options.output_height,
                    task.overview_values.data(), copied_options.output_width,
                    [&](double value) {
                        task.progress.store(
                            overview_progress_begin +
                            (1.0 - overview_progress_begin) * value);
                    },
                    cancelled);
                task.view_result_ready = true;
            });
    });
}

dt_status DT_CALL dt_grid_view_cache_create(
    dt_grid_handle grid, const dt_grid_view_cache_options* options,
    dt_grid_view_cache_handle* output_cache) {
    if (output_cache) *output_cache = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_view_cache_options");
        if (!output_cache) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_cache is null");
        }
        auto handle = std::make_unique<dt_grid_view_cache_t>();
        handle->cache = std::make_shared<dt::GridViewCache>(
            require_grid_shared(grid), *options);
        *output_cache = handle.release();
    });
}

dt_status DT_CALL dt_grid_read_view_cached_async(
    dt_grid_view_cache_handle cache,
    const dt_grid_view_request_options* options,
    dt_task_handle* output_task) {
    if (output_task) *output_task = nullptr;
    return guarded([&] {
        validate_options(options, "dt_grid_view_request_options");
        if (!output_task) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_task is null");
        }
        constexpr uint32_t kKnownFlags =
            DT_GRID_VIEW_REQUEST_STRICT_NODATA |
            DT_GRID_VIEW_REQUEST_USE_PYRAMID |
            DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE |
            DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS;
        if ((options->flags & ~kKnownFlags) != 0) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "unknown GRID view request flags");
        }
        const auto shared_cache = require_grid_view_cache_shared(cache);
        const auto copied_options = *options;
        *output_task = start_task(
            DT_TASK_RESULT_GRID_VIEW,
            [shared_cache, copied_options](dt_task_t& task) {
                auto result = shared_cache->read_view(
                    copied_options,
                    [&](double value) { task.progress.store(value); },
                    [&] { return task.cancellation_requested.load(); });
                task.view_window = result.source_window;
                task.overview_width = result.width;
                task.overview_height = result.height;
                task.overview_values = std::move(result.values);
                task.overview_result = result.overview;
                task.verification_result = result.verification;
                task.view_result_flags = result.flags;
                task.view_lod_scale = result.lod_scale;
                task.view_tile_count = result.tile_count;
                task.view_reused_tile_count = result.reused_tile_count;
                task.view_result_ready = true;
            });
    });
}

dt_status DT_CALL dt_grid_view_cache_get_statistics(
    dt_grid_view_cache_handle cache,
    dt_grid_view_cache_statistics* output_statistics) {
    if (output_statistics) *output_statistics = {};
    return guarded([&] {
        if (!output_statistics) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_statistics is null");
        }
        *output_statistics =
            require_grid_view_cache_shared(cache)->statistics();
    });
}

dt_status DT_CALL dt_grid_view_cache_clear(dt_grid_view_cache_handle cache) {
    return guarded([&] { require_grid_view_cache_shared(cache)->clear(); });
}

void DT_CALL dt_grid_view_cache_destroy(dt_grid_view_cache_handle cache) {
    delete cache;
}

dt_status DT_CALL dt_task_get_info(dt_task_handle task,
                                   dt_task_info* output_info) {
    return guarded([&] {
        if (!output_info) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_info is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        dt_task_info info{};
        info.struct_size = sizeof(info);
        info.state = required.state;
        info.result_kind = required.result_kind;
        info.result_status = required.result_status;
        info.progress = required.progress.load();
        info.cancellation_requested =
            required.cancellation_requested.load() ? 1 : 0;
        *output_info = info;
    });
}

dt_status DT_CALL dt_task_wait(dt_task_handle task,
                               uint32_t timeout_milliseconds,
                               int32_t* completed) {
    if (completed) *completed = 0;
    return guarded([&] {
        if (!completed) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "completed is null");
        }
        auto& required = require_task(task);
        std::unique_lock<std::mutex> lock(required.mutex);
        const auto predicate = [&] { return task_finished(required.state); };
        bool done = false;
        if (timeout_milliseconds == UINT32_MAX) {
            required.completed.wait(lock, predicate);
            done = true;
        } else {
            done = required.completed.wait_for(
                lock, std::chrono::milliseconds(timeout_milliseconds), predicate);
        }
        *completed = done ? 1 : 0;
    });
}

dt_status DT_CALL dt_task_request_cancel(dt_task_handle task) {
    return guarded([&] { require_task(task).cancellation_requested.store(true); });
}

dt_status DT_CALL dt_task_get_grid_result(dt_task_handle task,
                                          dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED || !required.grid_result) {
            throw dt::Exception(DT_E_NOT_FOUND,
                                "task has no completed GRID result");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = required.grid_result;
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_task_get_contour_result(
    dt_task_handle task, dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED || !required.contour_result) {
            throw dt::Exception(DT_E_NOT_FOUND,
                                "task has no completed contour result");
        }
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours = required.contour_result;
        *output_contours = result.release();
    });
}

dt_status DT_CALL dt_task_get_earthwork_result(
    dt_task_handle task, dt_grid_earthwork_result* output_result,
    dt_grid_handle* output_difference_grid) {
    if (output_result) *output_result = {};
    if (output_difference_grid) *output_difference_grid = nullptr;
    return guarded([&] {
        if (!output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_result is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED ||
            !required.earthwork_result_ready) {
            throw dt::Exception(DT_E_NOT_FOUND,
                                "task has no completed earthwork result");
        }
        *output_result = required.earthwork_result;
        if (output_difference_grid && required.grid_result) {
            auto result = std::make_unique<dt_grid_t>();
            result->grid = required.grid_result;
            *output_difference_grid = result.release();
        }
    });
}

dt_status DT_CALL dt_task_get_grid_overview_result(
    dt_task_handle task, dt_grid_overview_view* output_view) {
    if (output_view) *output_view = {};
    return guarded([&] {
        if (!output_view) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_view is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED ||
            !required.overview_result_ready) {
            throw dt::Exception(
                DT_E_NOT_FOUND,
                "task has no completed GRID overview result");
        }
        dt_grid_overview_view view{};
        view.struct_size = sizeof(view);
        view.width = required.overview_width;
        view.height = required.overview_height;
        view.row_stride = required.overview_width;
        view.values = required.overview_values.data();
        view.result = required.overview_result;
        *output_view = view;
    });
}

dt_status DT_CALL dt_task_get_grid_verification_result(
    dt_task_handle task, dt_grid_verify_result* output_result) {
    if (output_result) *output_result = {};
    return guarded([&] {
        if (!output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_result is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED ||
            !required.verification_result_ready) {
            throw dt::Exception(
                DT_E_NOT_FOUND,
                "task has no completed GRID verification result");
        }
        *output_result = required.verification_result;
    });
}

dt_status DT_CALL dt_task_get_grid_view_result(
    dt_task_handle task, dt_grid_view_result* output_result) {
    if (output_result) *output_result = {};
    return guarded([&] {
        if (!output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_result is null");
        }
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        if (required.state != DT_TASK_SUCCEEDED ||
            !required.view_result_ready) {
            throw dt::Exception(
                DT_E_NOT_FOUND,
                "task has no completed GRID view result");
        }
        dt_grid_view_result result{};
        result.struct_size = sizeof(result);
        result.flags = required.view_result_flags;
        result.source_window = required.view_window;
        result.width = required.overview_width;
        result.height = required.overview_height;
        result.row_stride = required.overview_width;
        result.values = required.overview_values.data();
        result.overview = required.overview_result;
        result.verification = required.verification_result;
        result.lod_scale = required.view_lod_scale;
        result.tile_count = required.view_tile_count;
        result.reused_tile_count = required.view_reused_tile_count;
        *output_result = result;
    });
}

dt_status DT_CALL dt_task_get_error(dt_task_handle task, char* buffer,
                                    size_t buffer_size, size_t* required_size) {
    return guarded([&] {
        auto& required = require_task(task);
        std::lock_guard<std::mutex> lock(required.mutex);
        const size_t needed = required.error.size() + 1;
        if (required_size) *required_size = needed;
        if (!buffer) {
            if (buffer_size != 0) {
                throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                    "buffer is null but buffer_size is nonzero");
            }
            return;
        }
        if (buffer_size == 0) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "buffer_size is zero");
        }
        const size_t copied = std::min(buffer_size - 1, required.error.size());
        std::memcpy(buffer, required.error.data(), copied);
        buffer[copied] = '\0';
        if (buffer_size < needed) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "task error buffer is too small");
        }
    });
}

void DT_CALL dt_task_destroy(dt_task_handle task) {
    if (!task) return;
    task->cancellation_requested.store(true);
    if (task->worker.joinable()) task->worker.join();
    delete task;
}

dt_status DT_CALL dt_create(const dt_options* options, dt_handle* out_handle) {
    if (out_handle) *out_handle = nullptr;
    return guarded([&] {
        if (!out_handle) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "out_handle is null");
        }
        if (options && options->struct_size != 0 &&
            options->struct_size < sizeof(dt_options)) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "dt_options has an incompatible struct_size");
        }
        *out_handle = new dt_context_t();
    });
}

void DT_CALL dt_destroy(dt_handle handle) {
    delete handle;
}

dt_status DT_CALL dt_clear_handle(dt_handle handle) {
    return guarded([&] { require_context(handle).clear(); });
}

dt_status DT_CALL dt_build(dt_handle handle, const dt_point3* points,
                           uint64_t point_count, dt_vertex_id* output_ids) {
    return guarded([&] {
        require_context(handle).build(points, point_count, output_ids);
    });
}

dt_status DT_CALL dt_insert_point(dt_handle handle, const dt_point3* point,
                                  dt_vertex_id* output_id,
                                  dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        if (!point) throw dt::Exception(DT_E_INVALID_ARGUMENT, "point is null");
        auto data = require_context(handle).insert(*point, output_id);
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_delete_nearest_xy(dt_handle handle, const dt_point3* query,
                                       dt_vertex_id* deleted_id,
                                       dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        if (!query) throw dt::Exception(DT_E_INVALID_ARGUMENT, "query is null");
        auto data = require_context(handle).delete_nearest(*query, deleted_id);
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_delete_vertex(dt_handle handle, dt_vertex_id vertex_id,
                                   dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        auto data = require_context(handle).delete_vertex(vertex_id);
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_update_vertex_z(dt_handle handle, dt_vertex_id vertex_id,
                                     double z) {
    return guarded([&] { require_context(handle).update_z(vertex_id, z); });
}

dt_status DT_CALL dt_find_nearest_vertex_xy(dt_handle handle,
                                            const dt_point3* query,
                                            dt_vertex3* output_vertex) {
    return guarded([&] {
        if (!query || !output_vertex) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_vertex is null");
        }
        *output_vertex = require_context(handle).nearest(*query);
    });
}

dt_status DT_CALL dt_locate_point_xy(dt_handle handle, const dt_point3* query,
                                     dt_location_result* output_result) {
    return guarded([&] {
        if (!query || !output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_result is null");
        }
        *output_result = require_context(handle).locate(*query);
    });
}

dt_status DT_CALL dt_analyze_tin_surface_xy(
    dt_handle handle, const dt_point3* query,
    dt_surface_analysis* output_analysis) {
    return guarded([&] {
        if (!query || !output_analysis) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_analysis is null");
        }
        *output_analysis = require_context(handle).analyze_surface_xy(*query);
    });
}

dt_status DT_CALL dt_query_triangles(dt_handle handle, const dt_bounds2* bounds,
                                     dt_query_result* output_result) {
    if (output_result) *output_result = nullptr;
    return guarded([&] {
        if (!bounds || !output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "bounds or output_result is null");
        }
        auto data = require_context(handle).query(*bounds);
        auto result = std::make_unique<dt_query_result_t>();
        result->data = std::move(*data);
        *output_result = result.release();
    });
}

dt_status DT_CALL dt_get_statistics(dt_handle handle,
                                    dt_statistics* output_statistics) {
    return guarded([&] {
        if (!output_statistics) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_statistics is null");
        }
        *output_statistics = require_context(handle).statistics();
    });
}

dt_status DT_CALL dt_validate(dt_handle handle, int32_t verbose) {
    return guarded([&] {
        if (!require_context(handle).validate(verbose != 0)) {
            throw dt::Exception(DT_E_INTERNAL, "triangulation validation failed");
        }
    });
}

dt_status DT_CALL dt_save(dt_handle handle, const char* utf8_file_name) {
    return guarded([&] { require_context(handle).save(utf8_file_name); });
}

dt_status DT_CALL dt_load(dt_handle handle, const char* utf8_file_name,
                          dt_bounds2* output_bounds) {
    return guarded([&] {
        const auto bounds = require_context(handle).load(utf8_file_name);
        if (output_bounds) *output_bounds = bounds;
    });
}

dt_status DT_CALL dt_import_points_text(dt_handle handle,
                                        const char* utf8_file_name,
                                        dt_bounds2* output_bounds) {
    return guarded([&] {
        const auto bounds =
            require_context(handle).import_points_text(utf8_file_name);
        if (output_bounds) *output_bounds = bounds;
    });
}

dt_status DT_CALL dt_save_mesh_text(dt_handle handle,
                                    const char* utf8_file_name) {
    return guarded(
        [&] { require_context(handle).save_mesh_text(utf8_file_name); });
}

dt_status DT_CALL dt_load_mesh_text(dt_handle handle,
                                    const char* utf8_file_name,
                                    dt_bounds2* output_bounds) {
    return guarded([&] {
        const auto bounds =
            require_context(handle).load_mesh_text(utf8_file_name);
        if (output_bounds) *output_bounds = bounds;
    });
}

dt_status DT_CALL dt_edit_result_get_view(dt_edit_result result,
                                          dt_edit_result_view* output_view) {
    return guarded([&] {
        if (!result || !output_view) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "result or output_view is null");
        }
        dt_edit_result_view view{};
        view.struct_size = sizeof(view);
        view.removed_triangles = result->data.removed_triangles.data();
        view.removed_triangle_count = result->data.removed_triangles.size();
        view.added_triangles = result->data.added_triangles.data();
        view.added_triangle_count = result->data.added_triangles.size();
        view.boundary_edges = result->data.boundary_edges.data();
        view.boundary_edge_count = result->data.boundary_edges.size();
        view.removed_edges = result->data.removed_edges.data();
        view.removed_edge_count = result->data.removed_edges.size();
        view.added_edges = result->data.added_edges.data();
        view.added_edge_count = result->data.added_edges.size();
        view.affected_vertex_id = result->data.affected_vertex_id;
        view.generation = result->data.generation;
        *output_view = view;
    });
}

void DT_CALL dt_release_edit_result(dt_edit_result result) {
    delete result;
}

dt_status DT_CALL dt_query_result_get_view(dt_query_result result,
                                           dt_query_result_view* output_view) {
    return guarded([&] {
        if (!result || !output_view) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "result or output_view is null");
        }
        dt_query_result_view view{};
        view.struct_size = sizeof(view);
        view.triangles = result->data.triangles.data();
        view.triangle_count = result->data.triangles.size();
        view.generation = result->data.generation;
        *output_view = view;
    });
}

void DT_CALL dt_release_query_result(dt_query_result result) {
    delete result;
}

dt_status DT_CALL dt_set_crs_wkt(dt_handle handle,
                                  const char* utf8_crs_wkt) {
    return guarded([&] {
        require_context(handle).set_crs_wkt(utf8_crs_wkt ? utf8_crs_wkt : "");
    });
}

dt_status DT_CALL dt_get_crs_wkt(dt_handle handle, char* buffer,
                                  size_t buffer_size,
                                  size_t* required_size) {
    return guarded([&] {
        const auto value = require_context(handle).crs_wkt();
        const dt_status status =
            copy_utf8_string(value, buffer, buffer_size, required_size);
        if (status != DT_OK) {
            throw dt::Exception(status, "CRS output buffer is too small");
        }
    });
}

dt_status DT_CALL dt_cdt_create(const dt_cdt_options* options,
                                dt_cdt_handle* output_cdt) {
    if (output_cdt) *output_cdt = nullptr;
    return guarded([&] {
        if (!output_cdt) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_cdt is null");
        }
        if (options) validate_options(options, "dt_cdt_options");
        *output_cdt = new dt_cdt_t();
    });
}

void DT_CALL dt_cdt_destroy(dt_cdt_handle cdt) {
    delete cdt;
}

dt_status DT_CALL dt_cdt_clear(dt_cdt_handle cdt) {
    return guarded([&] { require_cdt(cdt).clear(); });
}

dt_status DT_CALL dt_cdt_build(dt_cdt_handle cdt, const dt_point3* points,
                               uint64_t point_count) {
    return guarded([&] { require_cdt(cdt).build(points, point_count); });
}

dt_status DT_CALL dt_cdt_build_from_tin(dt_cdt_handle cdt, dt_handle tin) {
    return guarded([&] {
        auto& source = require_context(tin);
        auto points = source.points();
        require_cdt(cdt).build_from_tin(std::move(points), source.crs_wkt());
    });
}

dt_status DT_CALL dt_cdt_add_constraint(
    dt_cdt_handle cdt, int32_t kind, uint32_t flags,
    const dt_point3* points, uint64_t point_count,
    dt_constraint_id* output_constraint_id) {
    if (output_constraint_id) *output_constraint_id = 0;
    return guarded([&] {
        const auto id =
            require_cdt(cdt).add_constraint(kind, flags, points, point_count);
        if (output_constraint_id) *output_constraint_id = id;
    });
}

dt_status DT_CALL dt_cdt_update_constraint(
    dt_cdt_handle cdt, dt_constraint_id constraint_id, uint32_t flags,
    const dt_point3* points, uint64_t point_count,
    dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        auto data = require_cdt(cdt).update_constraint(
            constraint_id, flags, points, point_count,
            output_effect != nullptr);
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_cdt_get_constraint_vertex_usage(
    dt_cdt_handle cdt, dt_constraint_id constraint_id, uint64_t point_index,
    dt_cdt_vertex_usage* output_usage) {
    if (output_usage) *output_usage = {};
    return guarded([&] {
        if (!output_usage) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_usage is null");
        }
        *output_usage = require_cdt(cdt).constraint_vertex_usage(
            constraint_id, point_index);
    });
}

dt_status DT_CALL dt_cdt_remove_constraint_vertex(
    dt_cdt_handle cdt, dt_constraint_id constraint_id, uint64_t point_index,
    uint32_t flags, dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        auto data = require_cdt(cdt).remove_constraint_vertex(
            constraint_id, point_index, flags, output_effect != nullptr);
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_cdt_apply_constraint_edits(
    dt_cdt_handle cdt, const dt_cdt_constraint_edit* edits,
    uint64_t edit_count, dt_constraint_id* output_constraint_ids,
    dt_edit_result* output_effect) {
    if (output_effect) *output_effect = nullptr;
    return guarded([&] {
        std::vector<dt_constraint_id> ids;
        auto data = require_cdt(cdt).apply_constraint_edits(
            edits, edit_count, ids, output_effect != nullptr);
        if (output_constraint_ids) {
            std::copy(ids.begin(), ids.end(), output_constraint_ids);
        }
        if (output_effect) {
            auto result = std::make_unique<dt_edit_result_t>();
            result->data = std::move(*data);
            *output_effect = result.release();
        }
    });
}

dt_status DT_CALL dt_cdt_remove_constraint(dt_cdt_handle cdt,
                                            dt_constraint_id constraint_id) {
    return guarded(
        [&] { require_cdt(cdt).remove_constraint(constraint_id); });
}

dt_status DT_CALL dt_cdt_get_statistics(
    dt_cdt_handle cdt, dt_cdt_statistics* output_statistics) {
    return guarded([&] {
        if (!output_statistics) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_statistics is null");
        }
        *output_statistics = require_cdt(cdt).statistics();
    });
}

dt_status DT_CALL dt_cdt_get_constraint_info(
    dt_cdt_handle cdt, uint64_t constraint_index,
    dt_constraint_info* output_info) {
    return guarded([&] {
        if (!output_info) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_info is null");
        }
        const auto constraint = require_cdt(cdt).constraint_at(constraint_index);
        dt_constraint_info result{};
        result.struct_size = sizeof(result);
        result.flags = constraint.flags;
        result.id = constraint.id;
        result.kind = constraint.kind;
        result.point_count = constraint.points.size();
        *output_info = result;
    });
}

dt_status DT_CALL dt_cdt_copy_constraint_points(
    dt_cdt_handle cdt, dt_constraint_id constraint_id,
    dt_point3* output_points, uint64_t point_capacity,
    uint64_t* required_count) {
    if (required_count) *required_count = 0;
    return guarded([&] {
        const auto constraint = require_cdt(cdt).constraint_by_id(constraint_id);
        const uint64_t required = constraint.points.size();
        if (required_count) *required_count = required;
        if (!output_points) {
            if (point_capacity != 0) {
                throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                    "output_points is null");
            }
            return;
        }
        if (point_capacity < required) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "constraint point buffer is too small");
        }
        std::copy(constraint.points.begin(), constraint.points.end(),
                  output_points);
    });
}

dt_status DT_CALL dt_cdt_query_triangles(
    dt_cdt_handle cdt, const dt_bounds2* bounds,
    dt_cdt_query_result* output_result) {
    if (output_result) *output_result = nullptr;
    return guarded([&] {
        if (!bounds || !output_result) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "bounds or output_result is null");
        }
        auto data = require_cdt(cdt).query(*bounds);
        auto result = std::make_unique<dt_cdt_query_result_t>();
        result->data = std::move(*data);
        *output_result = result.release();
    });
}

dt_status DT_CALL dt_cdt_query_result_get_view(
    dt_cdt_query_result result, dt_cdt_query_result_view* output_view) {
    return guarded([&] {
        if (!result || !output_view) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "result or output_view is null");
        }
        dt_cdt_query_result_view view{};
        view.struct_size = sizeof(view);
        view.triangles = result->data.triangles.data();
        view.triangle_count = result->data.triangles.size();
        view.generation = result->data.generation;
        *output_view = view;
    });
}

void DT_CALL dt_cdt_release_query_result(dt_cdt_query_result result) {
    delete result;
}

dt_status DT_CALL dt_cdt_sample_height_xy(dt_cdt_handle cdt,
                                           const dt_point3* query,
                                           double* output_z) {
    return guarded([&] {
        if (!query || !output_z) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_z is null");
        }
        *output_z = require_cdt(cdt).sample_height_xy(*query);
    });
}

dt_status DT_CALL dt_cdt_analyze_surface_xy(
    dt_cdt_handle cdt, const dt_point3* query,
    dt_surface_analysis* output_analysis) {
    return guarded([&] {
        if (!query || !output_analysis) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "query or output_analysis is null");
        }
        *output_analysis = require_cdt(cdt).analyze_surface_xy(*query);
    });
}

dt_status DT_CALL dt_grid_from_cdt(
    dt_cdt_handle cdt, const dt_tin_to_grid_options* options,
    dt_grid_handle* output_grid) {
    if (output_grid) *output_grid = nullptr;
    return guarded([&] {
        validate_options(options, "dt_tin_to_grid_options");
        if (!output_grid) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT, "output_grid is null");
        }
        auto result = std::make_unique<dt_grid_t>();
        result->grid = dt::grid_from_cdt(require_cdt(cdt), *options);
        *output_grid = result.release();
    });
}

dt_status DT_CALL dt_contours_from_cdt(
    dt_cdt_handle cdt, const dt_contour_options* options,
    dt_contour_handle* output_contours) {
    if (output_contours) *output_contours = nullptr;
    return guarded([&] {
        validate_options(options, "dt_contour_options");
        if (!output_contours) {
            throw dt::Exception(DT_E_INVALID_ARGUMENT,
                                "output_contours is null");
        }
        auto result = std::make_unique<dt_contour_set_t>();
        result->contours = dt::contours_from_cdt(require_cdt(cdt), *options);
        *output_contours = result.release();
    });
}

dt_status DT_CALL dt_cdt_validate(dt_cdt_handle cdt, int32_t verbose) {
    return guarded([&] {
        if (!require_cdt(cdt).validate(verbose != 0)) {
            throw dt::Exception(DT_E_INTERNAL, "CDT validation failed");
        }
    });
}

dt_status DT_CALL dt_cdt_set_crs_wkt(dt_cdt_handle cdt,
                                     const char* utf8_crs_wkt) {
    return guarded([&] {
        require_cdt(cdt).set_crs_wkt(utf8_crs_wkt ? utf8_crs_wkt : "");
    });
}

dt_status DT_CALL dt_cdt_get_crs_wkt(dt_cdt_handle cdt, char* buffer,
                                     size_t buffer_size,
                                     size_t* required_size) {
    return guarded([&] {
        const auto value = require_cdt(cdt).crs_wkt();
        const dt_status status =
            copy_utf8_string(value, buffer, buffer_size, required_size);
        if (status != DT_OK) {
            throw dt::Exception(status, "CDT CRS output buffer is too small");
        }
    });
}

dt_status DT_CALL dt_cdt_save_text(dt_cdt_handle cdt,
                                   const char* utf8_file_name) {
    return guarded(
        [&] { require_cdt(cdt).save_text(utf8_file_name); });
}

dt_status DT_CALL dt_cdt_load_text(dt_cdt_handle cdt,
                                   const char* utf8_file_name,
                                   dt_bounds2* output_bounds) {
    return guarded([&] {
        const auto bounds = require_cdt(cdt).load_text(utf8_file_name);
        if (output_bounds) *output_bounds = bounds;
    });
}

dt_status DT_CALL dt_get_last_error(char* buffer, size_t buffer_size,
                                    size_t* required_size) {
    const auto& error = last_error();
    const size_t required = error.size() + 1;
    if (required_size) *required_size = required;
    if (!buffer) return buffer_size == 0 ? DT_OK : DT_E_INVALID_ARGUMENT;
    if (buffer_size == 0) return DT_E_INVALID_ARGUMENT;
    const size_t copied = std::min(buffer_size - 1, error.size());
    std::memcpy(buffer, error.data(), copied);
    buffer[copied] = '\0';
    return buffer_size >= required ? DT_OK : DT_E_INVALID_ARGUMENT;
}

} // extern "C"
