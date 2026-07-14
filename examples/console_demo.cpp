#include "dt_api.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    dt_handle mesh = nullptr;
    if (dt_create(nullptr, &mesh) != DT_OK) return 1;

    constexpr uint32_t side = 100;
    std::vector<dt_point3> points;
    points.reserve(side * side);
    for (uint32_t y = 0; y < side; ++y) {
        for (uint32_t x = 0; x < side; ++x) {
            const double px = static_cast<double>(x);
            const double py = static_cast<double>(y);
            const double z = 30.0 * std::sin(px * 0.05) * std::cos(py * 0.04);
            points.push_back({px, py, z});
        }
    }

    if (dt_build(mesh, points.data(), points.size(), nullptr) != DT_OK) {
        char message[512]{};
        dt_get_last_error(message, sizeof(message), nullptr);
        std::cerr << "Build failed: " << message << '\n';
        dt_destroy(mesh);
        return 2;
    }

    dt_statistics stats{};
    dt_get_statistics(mesh, &stats);
    std::cout << "vertices=" << stats.vertex_count
              << ", triangles=" << stats.finite_triangle_count << '\n';

    const dt_point3 point{50.25, 50.25, 15.0};
    dt_edit_result effect = nullptr;
    dt_vertex_id id = 0;
    if (dt_insert_point(mesh, &point, &id, &effect) == DT_OK) {
        dt_edit_result_view view{};
        dt_edit_result_get_view(effect, &view);
        std::cout << "inserted id=" << id
                  << ", removed faces=" << view.removed_triangle_count
                  << ", added faces=" << view.added_triangle_count << '\n';
        dt_release_edit_result(effect);
    }

    dt_destroy(mesh);
    return 0;
}

