#include <dterrain.hpp>

#include <array>
#include <cstdint>
#include <iostream>

int main() {
    uint32_t major = 0, minor = 0, patch = 0;
    dt_get_version(&major, &minor, &patch);

    auto terrain = dterrain::make_mesh();
    const std::array<dt_point3, 5> points{{
        {0.0, 0.0, 10.0}, {10.0, 0.0, 12.0}, {10.0, 10.0, 15.0},
        {0.0, 10.0, 11.0}, {5.0, 5.0, 14.0}}};
    dterrain::check(dt_build(terrain.get(), points.data(), points.size(), nullptr));

    dt_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    dterrain::check(dt_get_statistics(terrain.get(), &statistics));
    std::cout << "dterrain " << major << '.' << minor << '.' << patch
              << ": vertices=" << statistics.vertex_count
              << ", triangles=" << statistics.finite_triangle_count << '\n';
    return statistics.vertex_count == points.size() ? 0 : 2;
}
