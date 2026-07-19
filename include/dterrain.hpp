#ifndef DTERRAIN_HPP
#define DTERRAIN_HPP

#include "dterrain.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace dterrain {

inline std::string last_error() {
    size_t required = 0;
    (void)dt_get_last_error(nullptr, 0, &required);
    if (required <= 1) return "dterrain operation failed";
    std::vector<char> buffer(required, '\0');
    (void)dt_get_last_error(buffer.data(), buffer.size(), nullptr);
    return std::string(buffer.data());
}

inline void check(dt_status status) {
    if (status != DT_OK) throw std::runtime_error(last_error());
}

struct mesh_deleter {
    void operator()(dt_handle value) const noexcept { dt_destroy(value); }
};
struct grid_deleter {
    void operator()(dt_grid_handle value) const noexcept { dt_grid_destroy(value); }
};
struct contour_deleter {
    void operator()(dt_contour_handle value) const noexcept {
        dt_contours_destroy(value);
    }
};
struct cdt_deleter {
    void operator()(dt_cdt_handle value) const noexcept { dt_cdt_destroy(value); }
};
struct task_deleter {
    void operator()(dt_task_handle value) const noexcept { dt_task_destroy(value); }
};
struct view_cache_deleter {
    void operator()(dt_grid_view_cache_handle value) const noexcept {
        dt_grid_view_cache_destroy(value);
    }
};
struct edit_result_deleter {
    void operator()(dt_edit_result value) const noexcept {
        dt_release_edit_result(value);
    }
};
struct query_result_deleter {
    void operator()(dt_query_result value) const noexcept {
        dt_release_query_result(value);
    }
};
struct cdt_query_result_deleter {
    void operator()(dt_cdt_query_result value) const noexcept {
        dt_cdt_release_query_result(value);
    }
};
struct clip_result_deleter {
    void operator()(dt_surface_clip_result value) const noexcept {
        dt_surface_clip_result_destroy(value);
    }
};

template <class Handle, class Deleter>
using unique_handle =
    std::unique_ptr<typename std::remove_pointer<Handle>::type, Deleter>;

using mesh = unique_handle<dt_handle, mesh_deleter>;
using grid = unique_handle<dt_grid_handle, grid_deleter>;
using contours = unique_handle<dt_contour_handle, contour_deleter>;
using cdt = unique_handle<dt_cdt_handle, cdt_deleter>;
using task = unique_handle<dt_task_handle, task_deleter>;
using view_cache = unique_handle<dt_grid_view_cache_handle, view_cache_deleter>;
using edit_result = unique_handle<dt_edit_result, edit_result_deleter>;
using query_result = unique_handle<dt_query_result, query_result_deleter>;
using cdt_query_result =
    unique_handle<dt_cdt_query_result, cdt_query_result_deleter>;
using clip_result =
    unique_handle<dt_surface_clip_result, clip_result_deleter>;

inline mesh make_mesh(const dt_options* options = nullptr) {
    dt_handle value = nullptr;
    check(dt_create(options, &value));
    return mesh(value);
}

inline cdt make_cdt(const dt_cdt_options* options = nullptr) {
    dt_cdt_handle value = nullptr;
    check(dt_cdt_create(options, &value));
    return cdt(value);
}

inline grid make_grid(const dt_grid_create_options& options) {
    dt_grid_handle value = nullptr;
    check(dt_grid_create(&options, &value));
    return grid(value);
}

}  // namespace dterrain

#endif
