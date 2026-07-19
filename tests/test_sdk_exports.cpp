#include <iostream>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dterrain_export_tests <shared-library>\n";
        return 2;
    }

    static const char* required[] = {
        "dt_get_version", "dt_create", "dt_destroy", "dt_build",
        "dt_insert_point", "dt_delete_vertex", "dt_query_triangles",
        "dt_import_points_text", "dt_save_mesh_text", "dt_load_mesh_text",
        "dt_grid_create", "dt_grid_destroy", "dt_grid_read_window",
        "dt_grid_read_overview", "dt_grid_save_binary", "dt_grid_load_binary",
        "dt_grid_from_tin", "dt_tin_from_grid", "dt_contours_from_tin",
        "dt_contours_from_grid", "dt_tin_clip_polygon_exact",
        "dt_grid_register_surface", "dt_grid_compare_surface_adaptive",
        "dt_cdt_create", "dt_cdt_destroy", "dt_cdt_add_constraint",
        "dt_cdt_clip_polygon_exact", "dt_cdt_save_binary",
        "dt_task_destroy", "dt_task_get_info", "dt_get_last_error"};

#if defined(_WIN32)
    HMODULE library = LoadLibraryA(argv[1]);
    if (!library) {
        std::cerr << "cannot load DLL: " << GetLastError() << '\n';
        return 3;
    }
    int missing = 0;
    for (const char* name : required) {
        if (!GetProcAddress(library, name)) {
            std::cerr << "missing export: " << name << '\n';
            ++missing;
        }
    }
    FreeLibrary(library);
#else
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << "cannot load library: " << dlerror() << '\n';
        return 3;
    }
    int missing = 0;
    for (const char* name : required) {
        if (!dlsym(library, name)) {
            std::cerr << "missing export: " << name << '\n';
            ++missing;
        }
    }
    dlclose(library);
#endif
    if (missing) return 4;
    std::cout << "verified " << (sizeof(required) / sizeof(required[0]))
              << " required SDK exports\n";
    return 0;
}
