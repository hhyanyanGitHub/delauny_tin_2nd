#include "dt_api.h"
#include "dt_cdt_api.h"
#include "dt_gdal_api.h"
#include "dt_legacy.hpp"
#include "dt_task_api.h"
#include "dt_terrain_api.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
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

void test_cdt_api() {
    dt_cdt_handle cdt = nullptr;
    require_ok(dt_cdt_create(nullptr, &cdt), "dt_cdt_create");
    assert(cdt != nullptr);
    require_ok(dt_cdt_clear(cdt), "clear empty CDT");

    std::vector<dt_point3> terrain;
    for (int y = 0; y <= 6; ++y) {
        for (int x = 0; x <= 6; ++x) {
            terrain.push_back(
                {static_cast<double>(x), static_cast<double>(y),
                 100.0 + x * 2.0 + y * 3.0});
        }
    }
    require_ok(dt_cdt_build(cdt, terrain.data(), terrain.size()),
               "dt_cdt_build");

    const dt_point3 outer[] = {{0, 0, 100}, {6, 0, 112},
                               {6, 6, 130}, {0, 6, 118}};
    dt_constraint_id outer_id = 0;
    require_ok(dt_cdt_add_constraint(
                   cdt, DT_CONSTRAINT_OUTER_BOUNDARY, 0, outer, 4, &outer_id),
               "add outer boundary");
    assert(outer_id != 0);

    const dt_point3 hole[] = {{2, 2, 110}, {4, 2, 114},
                              {4, 4, 120}, {2, 4, 116}};
    dt_constraint_id hole_id = 0;
    require_ok(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_HOLE_BOUNDARY, 0,
                                     hole, 4, &hole_id),
               "add hole boundary");

    const dt_point3 breakline[] = {{0, 3, 109}, {1, 3, 111}, {2, 3, 113}};
    dt_constraint_id breakline_id = 0;
    require_ok(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                                     breakline, 3, &breakline_id),
               "add breakline");

    dt_cdt_statistics stats{};
    require_ok(dt_cdt_get_statistics(cdt, &stats), "CDT statistics");
    assert(stats.base_point_count == terrain.size());
    assert(stats.constraint_count == 3);
    assert(stats.constrained_edge_count >= 10);
    assert(stats.domain_triangle_count > 0);
    assert(stats.domain_triangle_count < stats.finite_triangle_count);
    assert(close(stats.bounds.xmin, 0.0) && close(stats.bounds.xmax, 6.0));
    require_ok(dt_cdt_validate(cdt, 0), "CDT validation");

    dt_constraint_info constraint_info{};
    require_ok(dt_cdt_get_constraint_info(cdt, 1, &constraint_info),
               "CDT constraint info");
    assert(constraint_info.id == hole_id);
    assert(constraint_info.kind == DT_CONSTRAINT_HOLE_BOUNDARY);
    assert((constraint_info.flags & DT_CONSTRAINT_CLOSED) != 0);
    assert(constraint_info.point_count == 4);
    uint64_t required_points = 0;
    require_ok(dt_cdt_copy_constraint_points(cdt, hole_id, nullptr, 0,
                                             &required_points),
               "CDT constraint point count");
    assert(required_points == 4);
    std::vector<dt_point3> copied(required_points);
    require_ok(dt_cdt_copy_constraint_points(cdt, hole_id, copied.data(),
                                             copied.size(), nullptr),
               "CDT copy constraint points");
    assert(close(copied[2].x, 4.0) && close(copied[2].y, 4.0));

    const dt_bounds2 all{-1, -1, 7, 7};
    dt_cdt_query_result query = nullptr;
    require_ok(dt_cdt_query_triangles(cdt, &all, &query),
               "CDT query triangles");
    dt_cdt_query_result_view view{};
    require_ok(dt_cdt_query_result_get_view(query, &view),
               "CDT query view");
    assert(view.triangle_count == stats.domain_triangle_count);
    for (uint64_t i = 0; i < view.triangle_count; ++i) {
        const auto& triangle = view.triangles[i];
        const double cx = (triangle.vertex[0].point.x +
                           triangle.vertex[1].point.x +
                           triangle.vertex[2].point.x) /
                          3.0;
        const double cy = (triangle.vertex[0].point.y +
                           triangle.vertex[1].point.y +
                           triangle.vertex[2].point.y) /
                          3.0;
        assert(!(cx > 2.0 && cx < 4.0 && cy > 2.0 && cy < 4.0));
    }
    dt_cdt_release_query_result(query);

    double sampled_z = 0.0;
    const dt_point3 surface_query{1.0, 1.0, -999.0};
    require_ok(dt_cdt_sample_height_xy(cdt, &surface_query, &sampled_z),
               "sample CDT height");
    assert(close(sampled_z, 105.0));
    const dt_point3 hole_query{3.0, 3.0, 0.0};
    assert(dt_cdt_sample_height_xy(cdt, &hole_query, &sampled_z) ==
           DT_E_NOT_FOUND);

    dt_tin_to_grid_options grid_options{};
    grid_options.struct_size = sizeof(grid_options);
    grid_options.width = 7;
    grid_options.height = 7;
    grid_options.geo_transform[0] = 0.0;
    grid_options.geo_transform[1] = 1.0;
    grid_options.geo_transform[3] = 0.0;
    grid_options.geo_transform[5] = 1.0;
    grid_options.nodata_value = -9999.0;
    dt_grid_handle cdt_grid = nullptr;
    require_ok(dt_grid_from_cdt(cdt, &grid_options, &cdt_grid),
               "CDT to GRID");
    dt_grid_info cdt_grid_info{};
    require_ok(dt_grid_get_info(cdt_grid, &cdt_grid_info), "CDT GRID info");
    assert(cdt_grid_info.width == 7 && cdt_grid_info.height == 7);
    assert(cdt_grid_info.valid_value_count == 48);
    std::vector<double> cdt_grid_values(49);
    require_ok(dt_grid_read_window(cdt_grid, 0, 0, 7, 7,
                                   cdt_grid_values.data(), 7),
               "read CDT GRID");
    assert(close(cdt_grid_values[0], 100.0));
    assert(close(cdt_grid_values[8], 105.0));
    assert(close(cdt_grid_values[24], -9999.0));
    dt_grid_destroy(cdt_grid);

    dt_contour_options cdt_contour_options{};
    cdt_contour_options.struct_size = sizeof(cdt_contour_options);
    cdt_contour_options.interval = 5.0;
    cdt_contour_options.base = 0.0;
    dt_contour_handle cdt_contours = nullptr;
    require_ok(dt_contours_from_cdt(cdt, &cdt_contour_options, &cdt_contours),
               "CDT contours");
    dt_contour_info cdt_contour_info{};
    require_ok(dt_contours_get_info(cdt_contours, &cdt_contour_info),
               "CDT contour info");
    assert(cdt_contour_info.line_count > 0);
    assert(cdt_contour_info.vertex_count >= cdt_contour_info.line_count * 2);
    dt_contours_destroy(cdt_contours);

    const dt_point3 crossing[] = {{1, 2.5, 109.5}, {1, 3.5, 112.5}};
    const auto crossing_status = dt_cdt_add_constraint(
        cdt, DT_CONSTRAINT_BREAKLINE, 0, crossing, 2, nullptr);
    assert(crossing_status == DT_E_UNSUPPORTED);
    dt_cdt_statistics after_failed_add{};
    require_ok(dt_cdt_get_statistics(cdt, &after_failed_add),
               "statistics after rejected crossing");
    assert(after_failed_add.constraint_count == stats.constraint_count);
    assert(after_failed_add.generation == stats.generation);

    const dt_point3 moved_breakline[] = {
        {0, 3, 109}, {1, 2.5, 109.5}, {2, 3, 113}};
    dt_edit_result cdt_effect = nullptr;
    require_ok(dt_cdt_update_constraint(
                   cdt, breakline_id, 0, moved_breakline, 3, &cdt_effect),
               "update breakline constraint");
    dt_edit_result_view cdt_effect_view{};
    require_ok(dt_edit_result_get_view(cdt_effect, &cdt_effect_view),
               "CDT edit effect view");
    assert(cdt_effect_view.removed_triangle_count > 0);
    assert(cdt_effect_view.added_triangle_count > 0);
    assert(cdt_effect_view.boundary_edge_count > 0);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after constraint update");
    assert(stats.constraint_count == 3);
    assert(stats.generation == after_failed_add.generation + 1);
    assert(cdt_effect_view.generation == stats.generation);
    dt_release_edit_result(cdt_effect);

    required_points = 0;
    require_ok(dt_cdt_copy_constraint_points(cdt, breakline_id, nullptr, 0,
                                             &required_points),
               "updated breakline point count");
    assert(required_points == 3);
    copied.resize(required_points);
    require_ok(dt_cdt_copy_constraint_points(cdt, breakline_id, copied.data(),
                                             copied.size(), nullptr),
               "copy updated breakline");
    assert(close(copied[1].x, 1.0) && close(copied[1].y, 2.5));

    const dt_point3 rejected_breakline[] = {
        {1, 2.5, 109.5}, {5, 2.5, 117.5}};
    const uint64_t generation_before_rejected_update = stats.generation;
    assert(dt_cdt_update_constraint(cdt, breakline_id, 0,
                                    rejected_breakline, 2, nullptr) ==
           DT_E_UNSUPPORTED);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after rejected constraint update");
    assert(stats.generation == generation_before_rejected_update);
    copied.resize(3);
    require_ok(dt_cdt_copy_constraint_points(cdt, breakline_id, copied.data(),
                                             copied.size(), nullptr),
               "constraint unchanged after rejected update");
    assert(close(copied[1].x, 1.0) && close(copied[1].y, 2.5));
    assert(dt_cdt_update_constraint(cdt, 999999, 0, moved_breakline, 3,
                                    nullptr) == DT_E_NOT_FOUND);

    const dt_point3 shared_breakline[] = {
        {1, 2.5, 109.5}, {1, 1, 105}};
    dt_constraint_id shared_breakline_id = 0;
    require_ok(dt_cdt_add_constraint(
                   cdt, DT_CONSTRAINT_BREAKLINE, 0, shared_breakline, 2,
                   &shared_breakline_id),
               "add breakline with shared vertex");
    dt_cdt_vertex_usage usage{};
    require_ok(dt_cdt_get_constraint_vertex_usage(cdt, breakline_id, 1,
                                                  &usage),
               "shared constraint vertex usage");
    assert(usage.struct_size == sizeof(usage));
    assert(close(usage.point.x, 1.0) && close(usage.point.y, 2.5));
    assert(usage.constraint_count == 2);
    assert(usage.reference_count == 2);
    assert(usage.is_base_point == 0);
    require_ok(dt_cdt_get_constraint_vertex_usage(cdt, shared_breakline_id, 1,
                                                  &usage),
               "base point constraint usage");
    assert(usage.constraint_count == 1);
    assert(usage.is_base_point == 1);

    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics before protected vertex removal");
    const uint64_t generation_before_protected_remove = stats.generation;
    assert(dt_cdt_remove_constraint_vertex(cdt, breakline_id, 1, 0,
                                           nullptr) == DT_E_UNSUPPORTED);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after protected vertex removal");
    assert(stats.generation == generation_before_protected_remove);

    cdt_effect = nullptr;
    require_ok(dt_cdt_remove_constraint_vertex(
                   cdt, breakline_id, 1,
                   DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH, &cdt_effect),
               "detach shared vertex from selected constraint");
    cdt_effect_view = {};
    require_ok(dt_edit_result_get_view(cdt_effect, &cdt_effect_view),
               "shared detach effect view");
    assert(cdt_effect_view.removed_triangle_count > 0);
    assert(cdt_effect_view.added_triangle_count > 0);
    dt_release_edit_result(cdt_effect);
    required_points = 0;
    require_ok(dt_cdt_copy_constraint_points(cdt, breakline_id, nullptr, 0,
                                             &required_points),
               "point count after shared detach");
    assert(required_points == 2);
    require_ok(dt_cdt_get_constraint_vertex_usage(cdt, shared_breakline_id, 0,
                                                  &usage),
               "usage after shared detach");
    assert(usage.constraint_count == 1 && usage.reference_count == 1);

    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics before minimum point rejection");
    const uint64_t generation_before_minimum_rejection = stats.generation;
    assert(dt_cdt_remove_constraint_vertex(cdt, shared_breakline_id, 0, 0,
                                           nullptr) == DT_E_INVALID_ARGUMENT);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after minimum point rejection");
    assert(stats.generation == generation_before_minimum_rejection);
    assert(dt_cdt_get_constraint_vertex_usage(cdt, breakline_id, 99,
                                              &usage) == DT_E_NOT_FOUND);

    const dt_point3 batch_updated_shared[] = {
        {1, 2.5, 109.5}, {1.5, 1.5, 107.5}, {1, 1, 105}};
    const dt_point3 batch_added_breakline[] = {
        {4.5, 1, 112}, {5.5, 1, 114}};
    dt_cdt_constraint_edit batch_edits[3]{};
    batch_edits[0].struct_size = sizeof(dt_cdt_constraint_edit);
    batch_edits[0].operation = DT_CDT_EDIT_UPDATE;
    batch_edits[0].constraint_id = shared_breakline_id;
    batch_edits[0].points = batch_updated_shared;
    batch_edits[0].point_count = 3;
    batch_edits[1].struct_size = sizeof(dt_cdt_constraint_edit);
    batch_edits[1].operation = DT_CDT_EDIT_ADD;
    batch_edits[1].kind = DT_CONSTRAINT_BREAKLINE;
    batch_edits[1].points = batch_added_breakline;
    batch_edits[1].point_count = 2;
    batch_edits[2].struct_size = sizeof(dt_cdt_constraint_edit);
    batch_edits[2].operation = DT_CDT_EDIT_REMOVE;
    batch_edits[2].constraint_id = breakline_id;
    dt_constraint_id batch_ids[3]{};
    cdt_effect = nullptr;
    const uint64_t generation_before_batch = stats.generation;
    require_ok(dt_cdt_apply_constraint_edits(
                   cdt, batch_edits, 3, batch_ids, &cdt_effect),
               "atomic constraint edit batch");
    assert(batch_ids[0] == shared_breakline_id);
    assert(batch_ids[1] != 0 && batch_ids[1] != shared_breakline_id);
    assert(batch_ids[2] == breakline_id);
    cdt_effect_view = {};
    require_ok(dt_edit_result_get_view(cdt_effect, &cdt_effect_view),
               "constraint batch effect view");
    assert(cdt_effect_view.removed_triangle_count > 0);
    assert(cdt_effect_view.added_triangle_count > 0);
    dt_release_edit_result(cdt_effect);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after constraint batch");
    assert(stats.constraint_count == 4);
    assert(stats.generation == generation_before_batch + 1);
    assert(cdt_effect_view.generation == stats.generation);
    assert(dt_cdt_copy_constraint_points(cdt, breakline_id, nullptr, 0,
                                         &required_points) == DT_E_NOT_FOUND);
    required_points = 0;
    require_ok(dt_cdt_copy_constraint_points(
                   cdt, shared_breakline_id, nullptr, 0, &required_points),
               "batch updated point count");
    assert(required_points == 3);

    const dt_point3 rejected_batch_update[] = {
        {1, 2.5, 109.5}, {1.25, 1.5, 107}, {1, 1, 105}};
    const dt_point3 rejected_batch_crossing[] = {
        {1, 2.5, 109.5}, {5, 2.5, 117.5}};
    dt_cdt_constraint_edit rejected_batch[2]{};
    rejected_batch[0].struct_size = sizeof(dt_cdt_constraint_edit);
    rejected_batch[0].operation = DT_CDT_EDIT_UPDATE;
    rejected_batch[0].constraint_id = shared_breakline_id;
    rejected_batch[0].points = rejected_batch_update;
    rejected_batch[0].point_count = 3;
    rejected_batch[1].struct_size = sizeof(dt_cdt_constraint_edit);
    rejected_batch[1].operation = DT_CDT_EDIT_ADD;
    rejected_batch[1].kind = DT_CONSTRAINT_BREAKLINE;
    rejected_batch[1].points = rejected_batch_crossing;
    rejected_batch[1].point_count = 2;
    dt_constraint_id rejected_ids[2] = {77, 88};
    const uint64_t generation_before_rejected_batch = stats.generation;
    assert(dt_cdt_apply_constraint_edits(cdt, rejected_batch, 2,
                                         rejected_ids, nullptr) ==
           DT_E_UNSUPPORTED);
    assert(rejected_ids[0] == 77 && rejected_ids[1] == 88);
    require_ok(dt_cdt_get_statistics(cdt, &stats),
               "statistics after rejected constraint batch");
    assert(stats.generation == generation_before_rejected_batch);
    copied.resize(3);
    require_ok(dt_cdt_copy_constraint_points(
                   cdt, shared_breakline_id, copied.data(), copied.size(),
                   nullptr),
               "batch rollback preserved earlier update");
    assert(close(copied[1].x, 1.5) && close(copied[1].y, 1.5));
    rejected_batch[0].struct_size = 0;
    assert(dt_cdt_apply_constraint_edits(cdt, rejected_batch, 1, nullptr,
                                         nullptr) == DT_E_INVALID_ARGUMENT);
    assert(dt_cdt_apply_constraint_edits(cdt, nullptr, 0, nullptr, nullptr) ==
           DT_E_INVALID_ARGUMENT);

    require_ok(dt_cdt_set_crs_wkt(cdt, "LOCAL_CS[\"CDT test\"]"),
               "set CDT CRS");
    const char* file_name = "dterrain_test_roundtrip.dcdt";
    require_ok(dt_cdt_save_text(cdt, file_name), "save DCDT text");
    dt_cdt_handle loaded = nullptr;
    require_ok(dt_cdt_create(nullptr, &loaded), "create loaded CDT");
    dt_bounds2 loaded_bounds{};
    require_ok(dt_cdt_load_text(loaded, file_name, &loaded_bounds),
               "load DCDT text");
    dt_cdt_statistics loaded_stats{};
    require_ok(dt_cdt_get_statistics(loaded, &loaded_stats),
               "loaded CDT statistics");
    assert(loaded_stats.base_point_count == stats.base_point_count);
    assert(loaded_stats.constraint_count == stats.constraint_count);
    assert(loaded_stats.domain_triangle_count == stats.domain_triangle_count);
    size_t crs_size = 0;
    require_ok(dt_cdt_get_crs_wkt(loaded, nullptr, 0, &crs_size),
               "loaded CDT CRS size");
    std::vector<char> crs(crs_size);
    require_ok(dt_cdt_get_crs_wkt(loaded, crs.data(), crs.size(), nullptr),
               "loaded CDT CRS");
    assert(std::string(crs.data()) == "LOCAL_CS[\"CDT test\"]");
    require_ok(dt_cdt_validate(loaded, 0), "loaded CDT validation");

    const char* invalid_id_file = "dterrain_test_invalid_cdt_id.dcdt";
    {
        std::ofstream invalid(invalid_id_file, std::ios::binary);
        invalid << "DCDT 1\nCRS \"\"\nPOINTS 0\nCONSTRAINTS 1\n"
                   "CONSTRAINT 18446744073709551615 1 0 2\n"
                   "0 0 0\n1 0 0\nEND\n";
    }
    const auto generation_before_invalid_load = loaded_stats.generation;
    assert(dt_cdt_load_text(loaded, invalid_id_file, nullptr) ==
           DT_E_CORRUPTED_DATA);
    require_ok(dt_cdt_get_statistics(loaded, &loaded_stats),
               "statistics after rejected DCDT id");
    assert(loaded_stats.generation == generation_before_invalid_load);
    assert(loaded_stats.constraint_count == stats.constraint_count);
    std::remove(invalid_id_file);

    const auto domain_before_remove = loaded_stats.domain_triangle_count;
    require_ok(dt_cdt_remove_constraint(loaded, hole_id),
               "remove hole constraint");
    require_ok(dt_cdt_get_statistics(loaded, &loaded_stats),
               "statistics after hole removal");
    assert(loaded_stats.constraint_count == 2);
    assert(loaded_stats.domain_triangle_count > domain_before_remove);

    dt_handle source_tin = nullptr;
    require_ok(dt_create(nullptr, &source_tin), "create CDT source TIN");
    require_ok(dt_build(source_tin, terrain.data(), terrain.size(), nullptr),
               "build CDT source TIN");
    require_ok(dt_set_crs_wkt(source_tin, "LOCAL_CS[\"source TIN\"]"),
               "set source TIN CRS");
    dt_cdt_handle copied_from_tin = nullptr;
    require_ok(dt_cdt_create(nullptr, &copied_from_tin),
               "create CDT copied from TIN");
    require_ok(dt_cdt_build_from_tin(copied_from_tin, source_tin),
               "build CDT from TIN");
    dt_cdt_statistics copied_stats{};
    require_ok(dt_cdt_get_statistics(copied_from_tin, &copied_stats),
               "copied CDT statistics");
    assert(copied_stats.base_point_count == terrain.size());
    assert(copied_stats.constraint_count == 0);
    size_t copied_crs_size = 0;
    require_ok(dt_cdt_get_crs_wkt(copied_from_tin, nullptr, 0,
                                  &copied_crs_size),
               "copied CDT CRS size");
    std::vector<char> copied_crs(copied_crs_size);
    require_ok(dt_cdt_get_crs_wkt(copied_from_tin, copied_crs.data(),
                                  copied_crs.size(), nullptr),
               "copied CDT CRS");
    assert(std::string(copied_crs.data()) == "LOCAL_CS[\"source TIN\"]");
    dt_cdt_destroy(copied_from_tin);
    dt_destroy(source_tin);

    dt_cdt_destroy(loaded);
    dt_cdt_destroy(cdt);
    std::remove(file_name);
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

void test_grid_and_contours() {
    uint32_t major = 0, minor = 0, patch = 0;
    dt_get_version(&major, &minor, &patch);
    assert(major == 0 && minor == 11 && patch == 0);

    dt_handle plane = nullptr;
    require_ok(dt_create(nullptr, &plane), "terrain create plane");
    const dt_point3 plane_points[] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 1.0},
        {1.0, 1.0, 2.0}, {0.0, 1.0, 1.0}};
    require_ok(dt_build(plane, plane_points, 4, nullptr), "terrain build plane");
    const char* test_crs = "LOCAL_CS[\"dterrain test\",UNIT[\"metre\",1]]";
    require_ok(dt_set_crs_wkt(plane, test_crs), "set TIN CRS");

    dt_tin_to_grid_options raster_options{};
    raster_options.struct_size = sizeof(raster_options);
    raster_options.width = 3;
    raster_options.height = 3;
    raster_options.geo_transform[0] = 0.0;
    raster_options.geo_transform[1] = 0.5;
    raster_options.geo_transform[5] = 0.5;
    raster_options.nodata_value = -9999.0;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_from_tin(plane, &raster_options, &grid),
               "dt_grid_from_tin");
    size_t crs_size = 0;
    require_ok(dt_grid_get_crs_wkt(grid, nullptr, 0, &crs_size),
               "GRID CRS size");
    std::vector<char> crs(crs_size);
    require_ok(dt_grid_get_crs_wkt(grid, crs.data(), crs.size(), nullptr),
               "GRID CRS");
    assert(std::string(crs.data()) == test_crs);

    dt_grid_info grid_info{};
    require_ok(dt_grid_get_info(grid, &grid_info), "dt_grid_get_info");
    assert(grid_info.width == 3 && grid_info.height == 3);
    assert(grid_info.valid_value_count == 9);
    double values[9]{};
    require_ok(dt_grid_read_window(grid, 0, 0, 3, 3, values, 0),
               "dt_grid_read_window");
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            assert(std::abs(values[row * 3 + column] -
                            (static_cast<double>(row + column) * 0.5)) < 1e-10);
        }
    }

    const char* grid_file = "dterrain_test_grid.dgrid";
    require_ok(dt_grid_save_text(grid, grid_file), "dt_grid_save_text");
    dt_grid_handle loaded_grid = nullptr;
    require_ok(dt_grid_load_text(grid_file, &loaded_grid), "dt_grid_load_text");
    double loaded_values[9]{};
    require_ok(dt_grid_read_window(loaded_grid, 0, 0, 3, 3, loaded_values, 3),
               "loaded grid window");
    for (size_t i = 0; i < 9; ++i) assert(close(values[i], loaded_values[i]));
    std::remove(grid_file);

    dt_contour_options contour_options{};
    contour_options.struct_size = sizeof(contour_options);
    contour_options.interval = 0.5;
    contour_options.base = 0.0;
    dt_contour_handle tin_contours = nullptr;
    require_ok(dt_contours_from_tin(plane, &contour_options, &tin_contours),
               "dt_contours_from_tin");
    dt_contour_info contour_info{};
    require_ok(dt_contours_get_info(tin_contours, &contour_info),
               "TIN contour info");
    assert(contour_info.line_count >= 3);
    assert(contour_info.vertex_count >= 6);
    for (uint64_t i = 0; i < contour_info.line_count; ++i) {
        dt_contour_line_view line{};
        require_ok(dt_contours_get_line(tin_contours, i, &line),
                   "TIN contour line");
        assert(line.point_count >= 2);
        for (uint64_t p = 0; p < line.point_count; ++p) {
            assert(close(line.points[p].z, line.elevation));
            assert(std::abs(line.points[p].x + line.points[p].y -
                            line.elevation) < 1e-10);
        }
    }

    dt_contour_handle grid_contours = nullptr;
    require_ok(dt_contours_from_grid(grid, &contour_options, &grid_contours),
               "dt_contours_from_grid");
    require_ok(dt_contours_get_info(grid_contours, &contour_info),
               "GRID contour info");
    assert(contour_info.line_count >= 3);

#if DT_WITH_GDAL
    require_ok(dt_gdal_initialize(), "GDAL initialize");
    int32_t available = 0;
    require_ok(dt_gdal_is_driver_available("GTiff", &available),
               "GTiff driver query");
    assert(available == 1);
    require_ok(dt_gdal_is_driver_available("COG", &available),
               "COG driver query");
    assert(available == 1);
    require_ok(dt_gdal_is_driver_available("GPKG", &available),
               "GPKG driver query");
    assert(available == 1);

    const char* tiff_file = "dterrain_test_grid.tif";
    const char* tiff_options[] = {"TILED=YES", "COMPRESS=DEFLATE", nullptr};
    dt_gdal_raster_save_options raster_save{};
    raster_save.struct_size = sizeof(raster_save);
    raster_save.driver_name = "GTiff";
    raster_save.creation_options = tiff_options;
    require_ok(dt_grid_save_gdal_raster(grid, tiff_file, &raster_save),
               "save GeoTIFF");
    dt_grid_handle gdal_grid = nullptr;
    require_ok(dt_grid_load_gdal_raster(tiff_file, nullptr, &gdal_grid),
               "load GeoTIFF");
    dt_grid_info gdal_info{};
    require_ok(dt_grid_get_info(gdal_grid, &gdal_info), "GeoTIFF info");
    assert(gdal_info.width == 3 && gdal_info.height == 3);
    for (int i = 0; i < 6; ++i) {
        assert(std::abs(gdal_info.geo_transform[i] -
                        grid_info.geo_transform[i]) < 1e-12);
    }
    double gdal_values[9]{};
    require_ok(dt_grid_read_window(gdal_grid, 0, 0, 3, 3, gdal_values, 3),
               "GeoTIFF values");
    for (size_t i = 0; i < 9; ++i) assert(close(values[i], gdal_values[i]));
    require_ok(dt_grid_get_crs_wkt(gdal_grid, nullptr, 0, &crs_size),
               "GeoTIFF CRS size");
    assert(crs_size > 1);
    dt_grid_destroy(gdal_grid);
    std::remove(tiff_file);

    const char* cog_file = "dterrain_test_grid_cog.tif";
    dt_gdal_raster_save_options cog_save{};
    cog_save.struct_size = sizeof(cog_save);
    cog_save.driver_name = "COG";
    require_ok(dt_grid_save_gdal_raster(grid, cog_file, &cog_save),
               "save COG");
    require_ok(dt_grid_load_gdal_raster(cog_file, nullptr, &gdal_grid),
               "load COG");
    require_ok(dt_grid_get_info(gdal_grid, &gdal_info), "COG info");
    assert(gdal_info.width == 3 && gdal_info.height == 3);
    dt_grid_destroy(gdal_grid);
    std::remove(cog_file);

    const char* gpkg_file = "dterrain_test_contours.gpkg";
    dt_gdal_contour_save_options contour_save{};
    contour_save.struct_size = sizeof(contour_save);
    require_ok(dt_contours_save_gdal_vector(grid_contours, gpkg_file,
                                             &contour_save),
               "save contour GeoPackage");
    dt_contour_handle gdal_contours = nullptr;
    require_ok(dt_contours_load_gdal_vector(gpkg_file, nullptr,
                                             &gdal_contours),
               "load contour GeoPackage");
    dt_contour_info gdal_contour_info{};
    require_ok(dt_contours_get_info(gdal_contours, &gdal_contour_info),
               "GeoPackage contour info");
    assert(gdal_contour_info.line_count == contour_info.line_count);
    assert(gdal_contour_info.vertex_count == contour_info.vertex_count);
    dt_contours_destroy(gdal_contours);
    std::remove(gpkg_file);
#else
    assert(dt_gdal_initialize() == DT_E_UNSUPPORTED);
#endif

    const char* contour_file = "dterrain_test_contours.dcontour";
    require_ok(dt_contours_save_text(grid_contours, contour_file),
               "dt_contours_save_text");
    dt_contour_handle loaded_contours = nullptr;
    require_ok(dt_contours_load_text(contour_file, &loaded_contours),
               "dt_contours_load_text");
    dt_contour_info loaded_contour_info{};
    require_ok(dt_contours_get_info(loaded_contours, &loaded_contour_info),
               "loaded contour info");
    assert(loaded_contour_info.line_count == contour_info.line_count);
    assert(loaded_contour_info.vertex_count == contour_info.vertex_count);
    std::remove(contour_file);

    dt_handle grid_tin = nullptr;
    require_ok(dt_create(nullptr, &grid_tin), "grid TIN create");
    dt_grid_to_tin_options tin_options{};
    tin_options.struct_size = sizeof(tin_options);
    require_ok(dt_tin_from_grid(grid, &tin_options, grid_tin),
               "dt_tin_from_grid");
    dt_statistics statistics{};
    require_ok(dt_get_statistics(grid_tin, &statistics), "grid TIN statistics");
    assert(statistics.vertex_count == 9);
    require_ok(dt_validate(grid_tin, 0), "grid TIN validate");

    dt_grid_create_options nodata_options{};
    nodata_options.struct_size = sizeof(nodata_options);
    nodata_options.flags = DT_GRID_HAS_NODATA;
    nodata_options.width = 2;
    nodata_options.height = 2;
    nodata_options.geo_transform[1] = 1.0;
    nodata_options.geo_transform[5] = 1.0;
    nodata_options.nodata_value = -9999.0;
    dt_grid_handle nodata_grid = nullptr;
    require_ok(dt_grid_create(&nodata_options, &nodata_grid),
               "NoData grid create");
    const double nodata_values[] = {0.0, 1.0, 1.0, -9999.0};
    require_ok(dt_grid_write_window(nodata_grid, 0, 0, 2, 2, nodata_values, 2),
               "NoData grid write");
    assert(dt_tin_from_grid(nodata_grid, &tin_options, grid_tin) ==
           DT_E_UNSUPPORTED);
    tin_options.flags = DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING;
    require_ok(dt_tin_from_grid(nodata_grid, &tin_options, grid_tin),
               "NoData grid bridging");
    require_ok(dt_get_statistics(grid_tin, &statistics),
               "NoData grid TIN statistics");
    assert(statistics.vertex_count == 3);

    dt_task_handle grid_task = nullptr;
    require_ok(dt_grid_from_tin_async(plane, &raster_options, &grid_task),
               "async grid start");
    /* The task retains the source context even after its public handle dies. */
    dt_destroy(plane);
    plane = nullptr;
    int32_t completed = 0;
    require_ok(dt_task_wait(grid_task, UINT32_MAX, &completed),
               "async grid wait");
    assert(completed == 1);
    dt_task_info task_info{};
    require_ok(dt_task_get_info(grid_task, &task_info), "async grid info");
    assert(task_info.state == DT_TASK_SUCCEEDED);
    assert(task_info.result_kind == DT_TASK_RESULT_GRID);
    assert(close(task_info.progress, 1.0));
    dt_grid_handle async_grid = nullptr;
    require_ok(dt_task_get_grid_result(grid_task, &async_grid),
               "async grid result");
    require_ok(dt_grid_get_info(async_grid, &grid_info), "async grid metadata");
    assert(grid_info.valid_value_count == 9);
    dt_grid_destroy(async_grid);
    dt_task_destroy(grid_task);

    const double explicit_levels[] = {0.25, 0.75, 1.25, 1.75};
    dt_contour_options async_contour_options{};
    async_contour_options.struct_size = sizeof(async_contour_options);
    async_contour_options.levels = explicit_levels;
    async_contour_options.level_count = 4;
    dt_task_handle contour_task = nullptr;
    require_ok(dt_contours_from_grid_async(
                   grid, &async_contour_options, &contour_task),
               "async contour start");
    require_ok(dt_task_wait(contour_task, UINT32_MAX, &completed),
               "async contour wait");
    require_ok(dt_task_get_info(contour_task, &task_info),
               "async contour info");
    assert(task_info.state == DT_TASK_SUCCEEDED);
    dt_contour_handle async_contours = nullptr;
    require_ok(dt_task_get_contour_result(contour_task, &async_contours),
               "async contour result");
    require_ok(dt_contours_get_info(async_contours, &contour_info),
               "async contour metadata");
    assert(contour_info.line_count == 4);
    dt_contours_destroy(async_contours);
    dt_task_destroy(contour_task);

    dt_contour_options invalid_contours{};
    invalid_contours.struct_size = sizeof(invalid_contours);
    dt_task_handle failed_task = nullptr;
    require_ok(dt_contours_from_grid_async(grid, &invalid_contours, &failed_task),
               "failed task start");
    require_ok(dt_task_wait(failed_task, UINT32_MAX, &completed),
               "failed task wait");
    require_ok(dt_task_get_info(failed_task, &task_info), "failed task info");
    assert(task_info.state == DT_TASK_FAILED);
    assert(task_info.result_status == DT_E_INVALID_ARGUMENT);
    size_t error_size = 0;
    require_ok(dt_task_get_error(failed_task, nullptr, 0, &error_size),
               "failed task error size");
    assert(error_size > 1);
    dt_task_destroy(failed_task);

    dt_grid_destroy(nodata_grid);
    dt_destroy(grid_tin);
    dt_contours_destroy(loaded_contours);
    dt_contours_destroy(grid_contours);
    dt_contours_destroy(tin_contours);
    dt_grid_destroy(loaded_grid);
    dt_grid_destroy(grid);
}

void test_packaged_terrain_samples() {
    const std::string root = DT_SOURCE_DIR;
    dt_grid_handle grid = nullptr;
    require_ok(dt_grid_load_text(
                   (root + "/examples/sample_grid.dgrid").c_str(), &grid),
               "load packaged GRID sample");
    dt_grid_info grid_info{};
    require_ok(dt_grid_get_info(grid, &grid_info), "packaged GRID info");
    assert(grid_info.width == 5 && grid_info.height == 5);
    assert(grid_info.valid_value_count == 25);
    dt_grid_destroy(grid);

    dt_contour_handle contours = nullptr;
    require_ok(dt_contours_load_text(
                   (root + "/examples/sample_contours.dcontour").c_str(),
                   &contours),
               "load packaged contour sample");
    dt_contour_info contour_info{};
    require_ok(dt_contours_get_info(contours, &contour_info),
               "packaged contour info");
    assert(contour_info.line_count == 2);
    assert(contour_info.vertex_count == 10);
    dt_contours_destroy(contours);

    dt_cdt_handle cdt = nullptr;
    require_ok(dt_cdt_create(nullptr, &cdt), "create packaged CDT");
    require_ok(dt_cdt_load_text(
                   cdt, (root + "/examples/sample_constraints.dcdt").c_str(),
                   nullptr),
               "load packaged CDT sample");
    dt_cdt_statistics cdt_info{};
    require_ok(dt_cdt_get_statistics(cdt, &cdt_info), "packaged CDT info");
    assert(cdt_info.base_point_count == 25);
    assert(cdt_info.constraint_count == 3);
    assert(cdt_info.domain_triangle_count > 0);
    assert(cdt_info.domain_triangle_count < cdt_info.finite_triangle_count);
    require_ok(dt_cdt_validate(cdt, 0), "packaged CDT validation");
    dt_cdt_destroy(cdt);
}

} // namespace

int main() {
    test_cdt_api();
    test_v2_api();
    test_legacy_api();
    test_random_dynamic_sequence();
    test_grid_and_contours();
    test_packaged_terrain_samples();
    std::cout << "All dterrain tests passed.\n";
    return 0;
}
