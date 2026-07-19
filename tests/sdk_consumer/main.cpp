#include <dterrain.hpp>

#include <array>

int main() {
    uint32_t major = 0;
    dt_get_version(&major, nullptr, nullptr);
    auto mesh = dterrain::make_mesh();
    const std::array<dt_point3, 4> points{{
        {0, 0, 0}, {1, 0, 1}, {1, 1, 2}, {0, 1, 1}}};
    dterrain::check(dt_build(mesh.get(), points.data(), points.size(), nullptr));
    dt_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    dterrain::check(dt_get_statistics(mesh.get(), &statistics));
    return major == 1 && statistics.vertex_count == 4 ? 0 : 1;
}
