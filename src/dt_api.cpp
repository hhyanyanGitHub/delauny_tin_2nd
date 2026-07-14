#include "dt_core.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

struct dt_context_t {
    dt::Context context;
};

struct dt_edit_result_t {
    dt::EditData data;
};

struct dt_query_result_t {
    dt::QueryData data;
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
    if (!handle) throw dt::Exception(DT_E_NOT_INITIALIZED, "invalid context handle");
    return handle->context;
}

} // namespace

extern "C" {

void DT_CALL dt_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major) *major = DT_VERSION_MAJOR;
    if (minor) *minor = DT_VERSION_MINOR;
    if (patch) *patch = DT_VERSION_PATCH;
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
