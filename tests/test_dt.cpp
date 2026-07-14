#include "dt_api.h"
#include "dt_legacy.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed with status " << status << ": " << error << '\n';
    std::abort();
}

bool close(double a, double b) {
    return std::abs(a - b) < 1e-12;
}

void test_v2_api() {
    dt_handle mesh = nullptr;
    require_ok(dt_create(nullptr, &mesh), "dt_create");
    assert(mesh != nullptr);

    const dt_point3 points[] = {
        {0.0, 0.0, 10.0}, {1.0, 0.0, 20.0}, {1.0, 1.0, 30.0},
        {0.0, 1.0, 40.0}, {0.5, 0.5, 50.0}};
    dt_vertex_id ids[5]{};
    require_ok(dt_build(mesh, points, 5, ids), "dt_build");
    for (size_t i = 0; i < 5; ++i) assert(ids[i] == i + 1);

    dt_statistics stats{};
    require_ok(dt_get_statistics(mesh, &stats), "dt_get_statistics");
    assert(stats.vertex_count == 5);
    assert(stats.finite_triangle_count == 4);
    assert(close(stats.bounds.xmin, 0.0) && close(stats.bounds.xmax, 1.0));
    require_ok(dt_validate(mesh, 0), "dt_validate after build");

    const dt_point3 nearest_query{0.02, 0.01, -1000.0};
    dt_vertex3 nearest{};
    require_ok(dt_find_nearest_vertex_xy(mesh, &nearest_query, &nearest),
               "dt_find_nearest_vertex_xy");
    assert(nearest.id == 1);
    assert(close(nearest.point.z, 10.0));

    const dt_point3 inside_query{0.30, 0.10, 0.0};
    dt_location_result location{};
    require_ok(dt_locate_point_xy(mesh, &inside_query, &location),
               "dt_locate_point_xy");
    assert(location.type == DT_LOCATION_FACE);

    const dt_bounds2 all{-1.0, -1.0, 2.0, 2.0};
    dt_query_result query = nullptr;
    require_ok(dt_query_triangles(mesh, &all, &query), "dt_query_triangles");
    dt_query_result_view query_view{};
    require_ok(dt_query_result_get_view(query, &query_view),
               "dt_query_result_get_view");
    assert(query_view.triangle_count == 4);
    dt_release_query_result(query);

    const dt_point3 inserted{0.31, 0.22, 77.0};
    dt_vertex_id inserted_id = 0;
    dt_edit_result insertion = nullptr;
    require_ok(dt_insert_point(mesh, &inserted, &inserted_id, &insertion),
               "dt_insert_point");
    assert(inserted_id != 0);
    dt_edit_result_view insertion_view{};
    require_ok(dt_edit_result_get_view(insertion, &insertion_view),
               "insertion result view");
    assert(insertion_view.added_triangle_count > 0);
    assert(insertion_view.added_edge_count > 0);
    dt_release_edit_result(insertion);
    require_ok(dt_validate(mesh, 0), "dt_validate after insertion");

    const auto duplicate_status = dt_insert_point(mesh, &inserted, nullptr, nullptr);
    assert(duplicate_status == DT_E_DUPLICATE_XY);

    dt_edit_result deletion = nullptr;
    require_ok(dt_delete_vertex(mesh, inserted_id, &deletion), "dt_delete_vertex");
    dt_edit_result_view deletion_view{};
    require_ok(dt_edit_result_get_view(deletion, &deletion_view),
               "deletion result view");
    assert(deletion_view.removed_triangle_count > 0);
    dt_release_edit_result(deletion);
    require_ok(dt_validate(mesh, 0), "dt_validate after deletion");

    query = nullptr;
    require_ok(dt_query_triangles(mesh, &all, &query),
               "dt_query_triangles after deletion");
    require_ok(dt_query_result_get_view(query, &query_view),
               "query result after deletion");
    assert(query_view.triangle_count == 4);
    dt_release_query_result(query);

    require_ok(dt_update_vertex_z(mesh, ids[0], 123.5), "dt_update_vertex_z");
    require_ok(dt_find_nearest_vertex_xy(mesh, &nearest_query, &nearest),
               "nearest after z update");
    assert(close(nearest.point.z, 123.5));

    const char* file_name = "dterrain_test_roundtrip.dtin";
    require_ok(dt_save(mesh, file_name), "dt_save");
    dt_handle loaded = nullptr;
    require_ok(dt_create(nullptr, &loaded), "dt_create loaded");
    dt_bounds2 loaded_bounds{};
    require_ok(dt_load(loaded, file_name, &loaded_bounds), "dt_load");
    require_ok(dt_get_statistics(loaded, &stats), "loaded statistics");
    assert(stats.vertex_count == 5);
    assert(stats.finite_triangle_count == 4);
    require_ok(dt_validate(loaded, 0), "loaded validation");
    dt_destroy(loaded);
    std::remove(file_name);

    const char* xyz_file = "dterrain_test_points.xyz";
    {
        std::ofstream xyz(xyz_file);
        xyz << "# x, y, z sample\n"
            << "0, 0, 10\n"
            << "1 0 20\n"
            << "1;1;30\n"
            << "0\t1\t40\n"
            << "0.5, 0.5, 50 # center\n";
    }
    dt_handle text_mesh = nullptr;
    require_ok(dt_create(nullptr, &text_mesh), "dt_create text mesh");
    require_ok(dt_import_points_text(text_mesh, xyz_file, &loaded_bounds),
               "dt_import_points_text");
    require_ok(dt_get_statistics(text_mesh, &stats), "point text statistics");
    assert(stats.vertex_count == 5);
    assert(stats.finite_triangle_count == 4);
    require_ok(dt_validate(text_mesh, 0), "point text validation");

    const char* text_mesh_file = "dterrain_test_roundtrip.dtmesh";
    require_ok(dt_save_mesh_text(text_mesh, text_mesh_file),
               "dt_save_mesh_text");
    dt_handle text_loaded = nullptr;
    require_ok(dt_create(nullptr, &text_loaded), "dt_create text loaded");
    require_ok(dt_load_mesh_text(text_loaded, text_mesh_file, &loaded_bounds),
               "dt_load_mesh_text");
    require_ok(dt_get_statistics(text_loaded, &stats),
               "loaded text mesh statistics");
    assert(stats.vertex_count == 5);
    assert(stats.finite_triangle_count == 4);
    require_ok(dt_validate(text_loaded, 0), "loaded text mesh validation");
    require_ok(dt_find_nearest_vertex_xy(text_loaded, &nearest_query, &nearest),
               "text mesh nearest");
    assert(close(nearest.point.z, 10.0));
    dt_destroy(text_loaded);
    dt_destroy(text_mesh);
    std::remove(text_mesh_file);
    std::remove(xyz_file);

    dt_destroy(mesh);
}

void test_legacy_api() {
    dt_init_dll();
    assert(dt_insert_a_point(0.0, 0.0, 1.0));
    assert(dt_insert_a_point(1.0, 0.0, 2.0));
    assert(dt_insert_a_point(0.0, 1.0, 3.0));

    double* triangles = nullptr;
    int triangle_count = 0;
    dt_view_to_range(triangles, triangle_count, -1.0, -1.0, 2.0, 2.0);
    assert(triangle_count == 1);
    assert(triangles != nullptr);

    double gx = 0.0, gy = 0.0, gz = 0.0;
    assert(dt_get_a_point_nearest_point(gx, gy, gz, 0.01, 0.01, 999.0));
    assert(close(gx, 0.0) && close(gy, 0.0) && close(gz, 1.0));

    double x0, y0, z0, x1, y1, z1, x2, y2, z2;
    assert(dt_get_a_triangle_covers_point(
        x0, y0, z0, x1, y1, z1, x2, y2, z2, 0.2, 0.2, 0.0));

    int f = 0, h = 0, e = 0;
    double* effect = nullptr;
    assert(dt_delete_a_point_with_draw(f, h, e, effect, 0.0, 0.0, 0.0));
    assert(f == 1);
    dt_free_dll();
}

void test_random_dynamic_sequence() {
    dt_handle mesh = nullptr;
    require_ok(dt_create(nullptr, &mesh), "random create");

    std::mt19937_64 random(0xD37A1AULL);
    std::uniform_real_distribution<double> coordinate(-1000.0, 1000.0);
    std::vector<dt_point3> points;
    points.reserve(500);
    for (size_t i = 0; i < 500; ++i) {
        const double x = coordinate(random) + static_cast<double>(i) * 1e-9;
        const double y = coordinate(random) - static_cast<double>(i) * 1e-9;
        points.push_back({x, y, std::sin(x * 0.01) * 20.0});
    }
    std::vector<dt_vertex_id> ids(points.size());
    require_ok(dt_build(mesh, points.data(), points.size(), ids.data()),
               "random build");

    for (size_t i = 0; i < 100; ++i) {
        require_ok(dt_delete_vertex(mesh, ids[i * 3], nullptr),
                   "random delete by ID");
        const dt_point3 inserted{coordinate(random) + 3000.0,
                                 coordinate(random) + 3000.0,
                                 static_cast<double>(i)};
        require_ok(dt_insert_point(mesh, &inserted, nullptr, nullptr),
                   "random insertion");
        if ((i + 1) % 10 == 0) {
            require_ok(dt_validate(mesh, 0), "random sequence validation");
        }
    }

    dt_statistics stats{};
    require_ok(dt_get_statistics(mesh, &stats), "random statistics");
    assert(stats.vertex_count == 500);
    const dt_bounds2 all{-5000.0, -5000.0, 5000.0, 5000.0};
    dt_query_result query = nullptr;
    require_ok(dt_query_triangles(mesh, &all, &query), "random full query");
    dt_query_result_view view{};
    require_ok(dt_query_result_get_view(query, &view), "random query view");
    assert(view.triangle_count == stats.finite_triangle_count);
    dt_release_query_result(query);
    dt_destroy(mesh);
}

} // namespace

int main() {
    test_v2_api();
    test_legacy_api();
    test_random_dynamic_sequence();
    std::cout << "All dterrain tests passed.\n";
    return 0;
}
