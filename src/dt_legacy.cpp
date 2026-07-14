#include "dt_legacy.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

namespace {

std::mutex g_legacy_mutex;
dt_handle g_default_handle = nullptr;
thread_local std::vector<double>* g_effect_buffer_ptr = nullptr;
thread_local std::vector<double>* g_view_buffer_ptr = nullptr;

std::vector<double>& effect_buffer() {
    if (!g_effect_buffer_ptr) g_effect_buffer_ptr = new std::vector<double>();
    return *g_effect_buffer_ptr;
}

std::vector<double>& view_buffer() {
    if (!g_view_buffer_ptr) g_view_buffer_ptr = new std::vector<double>();
    return *g_view_buffer_ptr;
}

void append_point(std::vector<double>& target, const dt_point3& point) {
    target.push_back(point.x);
    target.push_back(point.y);
    target.push_back(point.z);
}

bool flatten_effect(dt_edit_result effect, int& f, int& h, int& e,
                    double*& pointer) {
    f = h = e = 0;
    pointer = nullptr;
    dt_edit_result_view view{};
    if (dt_edit_result_get_view(effect, &view) != DT_OK) return false;
    if (view.removed_triangle_count > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        view.boundary_edge_count > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        view.added_edge_count > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    auto& buffer = effect_buffer();
    buffer.clear();
    const uint64_t point_count = view.removed_triangle_count * 3 +
                                 (view.boundary_edge_count + view.added_edge_count) * 2;
    if (point_count <= std::numeric_limits<size_t>::max() / 3) {
        buffer.reserve(static_cast<size_t>(point_count * 3));
    }
    for (uint64_t i = 0; i < view.removed_triangle_count; ++i) {
        for (const auto& vertex : view.removed_triangles[i].vertex) {
            append_point(buffer, vertex.point);
        }
    }
    for (uint64_t i = 0; i < view.boundary_edge_count; ++i) {
        append_point(buffer, view.boundary_edges[i].vertex[0].point);
        append_point(buffer, view.boundary_edges[i].vertex[1].point);
    }
    for (uint64_t i = 0; i < view.added_edge_count; ++i) {
        append_point(buffer, view.added_edges[i].vertex[0].point);
        append_point(buffer, view.added_edges[i].vertex[1].point);
    }
    f = static_cast<int>(view.removed_triangle_count);
    h = static_cast<int>(view.boundary_edge_count);
    e = static_cast<int>(view.added_edge_count);
    pointer = buffer.empty() ? nullptr : buffer.data();
    return true;
}

} // namespace

extern "C" {

void DT_CALL dt_init_dll() {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (!g_default_handle) dt_create(nullptr, &g_default_handle);
}

void DT_CALL dt_free_dll() {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    dt_destroy(g_default_handle);
    g_default_handle = nullptr;
}

bool DT_CALL dt_insert_a_point_with_draw(
    const double& x, const double& y, const double& z,
    int& f, int& h, int& e, double*& pEffect) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    f = h = e = 0;
    pEffect = nullptr;
    if (!g_default_handle) return false;
    const dt_point3 point{x, y, z};
    dt_edit_result effect = nullptr;
    const auto status = dt_insert_point(g_default_handle, &point, nullptr, &effect);
    if (status != DT_OK) return false;
    const bool ok = flatten_effect(effect, f, h, e, pEffect);
    dt_release_edit_result(effect);
    return ok;
}

bool DT_CALL dt_insert_a_point(
    const double& x, const double& y, const double& z) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (!g_default_handle) return false;
    const dt_point3 point{x, y, z};
    return dt_insert_point(g_default_handle, &point, nullptr, nullptr) == DT_OK;
}

bool DT_CALL dt_delete_a_point_with_draw(
    int& f, int& h, int& e, double*& pEffect,
    const double& x, const double& y, const double& z) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    f = h = e = 0;
    pEffect = nullptr;
    if (!g_default_handle) return false;
    const dt_point3 point{x, y, z};
    dt_edit_result effect = nullptr;
    const auto status = dt_delete_nearest_xy(g_default_handle, &point, nullptr, &effect);
    if (status != DT_OK) return false;
    const bool ok = flatten_effect(effect, f, h, e, pEffect);
    dt_release_edit_result(effect);
    return ok;
}

bool DT_CALL dt_delete_a_point(
    const double& x, const double& y, const double& z) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (!g_default_handle) return false;
    const dt_point3 point{x, y, z};
    return dt_delete_nearest_xy(g_default_handle, &point, nullptr, nullptr) == DT_OK;
}

void DT_CALL dt_clear() {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (g_default_handle) dt_clear_handle(g_default_handle);
}

void DT_CALL dt_save_triangulation(const char* fileName) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (g_default_handle) dt_save(g_default_handle, fileName);
}

void DT_CALL dt_load_triangulation(
    const char* fileName, double& xmin, double& ymin, double& xmax, double& ymax) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    xmin = ymin = xmax = ymax = 0.0;
    if (!g_default_handle) return;
    dt_bounds2 bounds{};
    if (dt_load(g_default_handle, fileName, &bounds) == DT_OK) {
        xmin = bounds.xmin;
        ymin = bounds.ymin;
        xmax = bounds.xmax;
        ymax = bounds.ymax;
    }
}

void DT_CALL dt_view_to_range(
    double*& pShowTri, int& TriNum,
    const double& xmin, const double& ymin,
    const double& xmax, const double& ymax) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    pShowTri = nullptr;
    TriNum = 0;
    if (!g_default_handle) return;
    const dt_bounds2 bounds{xmin, ymin, xmax, ymax};
    dt_query_result query = nullptr;
    if (dt_query_triangles(g_default_handle, &bounds, &query) != DT_OK) return;
    dt_query_result_view view{};
    if (dt_query_result_get_view(query, &view) == DT_OK &&
        view.triangle_count <= static_cast<uint64_t>(std::numeric_limits<int>::max()) &&
        view.triangle_count <= std::numeric_limits<size_t>::max() / 9) {
        auto& buffer = view_buffer();
        buffer.clear();
        buffer.reserve(static_cast<size_t>(view.triangle_count * 9));
        for (uint64_t i = 0; i < view.triangle_count; ++i) {
            for (const auto& vertex : view.triangles[i].vertex) {
                append_point(buffer, vertex.point);
            }
        }
        TriNum = static_cast<int>(view.triangle_count);
        pShowTri = buffer.empty() ? nullptr : buffer.data();
    }
    dt_release_query_result(query);
}

bool DT_CALL dt_get_a_point_nearest_point(
    double& gx, double& gy, double& gz,
    const double& x, const double& y, const double& z) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (!g_default_handle) return false;
    const dt_point3 query{x, y, z};
    dt_vertex3 vertex{};
    if (dt_find_nearest_vertex_xy(g_default_handle, &query, &vertex) != DT_OK) {
        return false;
    }
    gx = vertex.point.x;
    gy = vertex.point.y;
    gz = vertex.point.z;
    return true;
}

bool DT_CALL dt_get_a_triangle_covers_point(
    double& gx0, double& gy0, double& gz0,
    double& gx1, double& gy1, double& gz1,
    double& gx2, double& gy2, double& gz2,
    const double& x, const double& y, const double& z) {
    std::lock_guard<std::mutex> lock(g_legacy_mutex);
    if (!g_default_handle) return false;
    const dt_point3 query{x, y, z};
    dt_location_result result{};
    if (dt_locate_point_xy(g_default_handle, &query, &result) != DT_OK ||
        result.type != DT_LOCATION_FACE) {
        return false;
    }
    gx0 = result.triangle.vertex[0].point.x;
    gy0 = result.triangle.vertex[0].point.y;
    gz0 = result.triangle.vertex[0].point.z;
    gx1 = result.triangle.vertex[1].point.x;
    gy1 = result.triangle.vertex[1].point.y;
    gz1 = result.triangle.vertex[1].point.z;
    gx2 = result.triangle.vertex[2].point.x;
    gy2 = result.triangle.vertex[2].point.y;
    gz2 = result.triangle.vertex[2].point.z;
    return true;
}

} // extern "C"
