#include "dt_api.h"

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
    dt_destroy(mesh);
    return valid == DT_OK && insert_status == DT_OK && query_status == DT_OK &&
                   delete_status == DT_OK
               ? 0
               : 3;
}
