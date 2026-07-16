#include "dt_api.h"
#include "dt_cdt_api.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

int main(int argc, char** argv) {
    uint64_t count = 100000;
    if (argc > 1) count = std::strtoull(argv[1], nullptr, 10);
    if (count < 3) count = 3;
    uint64_t constraint_count = 0;
    if (argc > 2) constraint_count = std::strtoull(argv[2], nullptr, 10);

    std::vector<dt_point3> points;
    points.reserve(static_cast<size_t>(count));
    std::mt19937_64 random(20260714ULL);
    std::uniform_real_distribution<double> xy(0.0, 100000.0);
    for (uint64_t i = 0; i < count; ++i) {
        const double x = xy(random) + static_cast<double>(i) * 1e-10;
        const double y = xy(random) - static_cast<double>(i) * 1e-10;
        const double z = 200.0 * std::sin(x * 0.0001) * std::cos(y * 0.0001);
        points.push_back({x, y, z});
    }

    dt_handle mesh = nullptr;
    if (dt_create(nullptr, &mesh) != DT_OK) return 1;
    const auto begin = std::chrono::steady_clock::now();
    const auto status = dt_build(mesh, points.data(), points.size(), nullptr);
    const auto end = std::chrono::steady_clock::now();
    if (status != DT_OK) {
        char message[512]{};
        dt_get_last_error(message, sizeof(message), nullptr);
        std::cerr << "build failed: " << message << '\n';
        dt_destroy(mesh);
        return 2;
    }

    dt_statistics stats{};
    dt_get_statistics(mesh, &stats);
    const double seconds = std::chrono::duration<double>(end - begin).count();
    std::cout << "points=" << stats.vertex_count
              << " triangles=" << stats.finite_triangle_count
              << " build_seconds=" << seconds
              << " points_per_second=" << (stats.vertex_count / seconds) << '\n';
    const auto validation_begin = std::chrono::steady_clock::now();
    const auto valid = dt_validate(mesh, 0);
    const auto validation_end = std::chrono::steady_clock::now();
    std::cout << "validation_status=" << valid
              << " validation_seconds="
              << std::chrono::duration<double>(validation_end - validation_begin).count()
              << '\n';

    const dt_point3 inserted{50000.1234567, 50000.7654321, 88.0};
    dt_vertex_id inserted_id = 0;
    dt_edit_result effect = nullptr;
    const auto insert_begin = std::chrono::steady_clock::now();
    const auto insert_status = dt_insert_point(mesh, &inserted, &inserted_id, &effect);
    const auto insert_end = std::chrono::steady_clock::now();
    if (effect) dt_release_edit_result(effect);
    std::cout << "insert_status=" << insert_status
              << " insert_ms="
              << std::chrono::duration<double, std::milli>(insert_end - insert_begin).count()
              << '\n';

    const dt_bounds2 window{49900.0, 49900.0, 50100.0, 50100.0};
    dt_query_result query = nullptr;
    const auto query_begin = std::chrono::steady_clock::now();
    const auto query_status = dt_query_triangles(mesh, &window, &query);
    const auto query_end = std::chrono::steady_clock::now();
    uint64_t query_count = 0;
    if (query) {
        dt_query_result_view view{};
        dt_query_result_get_view(query, &view);
        query_count = view.triangle_count;
        dt_release_query_result(query);
    }
    std::cout << "query_status=" << query_status
              << " query_triangles=" << query_count
              << " query_ms="
              << std::chrono::duration<double, std::milli>(query_end - query_begin).count()
              << '\n';

    const auto delete_begin = std::chrono::steady_clock::now();
    const auto delete_status = dt_delete_vertex(mesh, inserted_id, nullptr);
    const auto delete_end = std::chrono::steady_clock::now();
    std::cout << "delete_status=" << delete_status
              << " delete_ms="
              << std::chrono::duration<double, std::milli>(delete_end - delete_begin).count()
              << '\n';

    bool cdt_ok = true;
    if (constraint_count > 0) {
        std::vector<std::array<dt_point3, 2>> lines(
            static_cast<size_t>(constraint_count));
        std::vector<dt_cdt_constraint_edit> edits(
            static_cast<size_t>(constraint_count));
        for (uint64_t i = 0; i < constraint_count; ++i) {
            const double fraction = static_cast<double>(i + 1) /
                                    static_cast<double>(constraint_count + 1);
            const double y = 10000.0 + 80000.0 * fraction;
            const double x0 = 10000.0;
            const double x1 = 90000.0;
            lines[static_cast<size_t>(i)] = {
                dt_point3{x0, y, 200.0 * std::sin(x0 * 0.0001) *
                                      std::cos(y * 0.0001)},
                dt_point3{x1, y, 200.0 * std::sin(x1 * 0.0001) *
                                      std::cos(y * 0.0001)}};
            auto& edit = edits[static_cast<size_t>(i)];
            edit.struct_size = sizeof(edit);
            edit.operation = DT_CDT_EDIT_ADD;
            edit.kind = DT_CONSTRAINT_BREAKLINE;
            edit.points = lines[static_cast<size_t>(i)].data();
            edit.point_count = 2;
        }

        dt_cdt_handle sequential = nullptr;
        dt_cdt_create(nullptr, &sequential);
        cdt_ok = dt_cdt_build(sequential, points.data(), points.size()) == DT_OK;
        const auto sequential_begin = std::chrono::steady_clock::now();
        for (uint64_t i = 0; cdt_ok && i < constraint_count; ++i) {
            const auto& line = lines[static_cast<size_t>(i)];
            cdt_ok = dt_cdt_add_constraint(
                         sequential, DT_CONSTRAINT_BREAKLINE, 0, line.data(),
                         line.size(), nullptr) == DT_OK;
        }
        const auto sequential_end = std::chrono::steady_clock::now();
        const double sequential_seconds =
            std::chrono::duration<double>(sequential_end - sequential_begin)
                .count();
        dt_cdt_destroy(sequential);

        dt_cdt_handle batched = nullptr;
        dt_cdt_create(nullptr, &batched);
        cdt_ok = cdt_ok &&
                 dt_cdt_build(batched, points.data(), points.size()) == DT_OK;
        std::vector<dt_constraint_id> ids(
            static_cast<size_t>(constraint_count));
        const auto batch_begin = std::chrono::steady_clock::now();
        const auto batch_status = dt_cdt_apply_constraint_edits(
            batched, edits.data(), edits.size(), ids.data(), nullptr);
        const auto batch_end = std::chrono::steady_clock::now();
        const double batch_seconds =
            std::chrono::duration<double>(batch_end - batch_begin).count();
        cdt_ok = cdt_ok && batch_status == DT_OK &&
                 dt_cdt_validate(batched, 0) == DT_OK;
        dt_cdt_destroy(batched);
        std::cout << "constraint_edits=" << constraint_count
                  << " sequential_seconds=" << sequential_seconds
                  << " batch_seconds=" << batch_seconds
                  << " speedup="
                  << (batch_seconds > 0.0
                          ? sequential_seconds / batch_seconds
                          : 0.0)
                  << '\n';
    }
    dt_destroy(mesh);
    return valid == DT_OK && insert_status == DT_OK && query_status == DT_OK &&
                   delete_status == DT_OK && cdt_ok
               ? 0
               : 3;
}
