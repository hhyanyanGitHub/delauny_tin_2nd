#include <dterrain.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

double seconds(clock_type::time_point begin) {
    return std::chrono::duration<double>(clock_type::now() - begin).count();
}

uint64_t parse_u64(const char* text, uint64_t fallback) {
    if (!text) return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end && *end == '\0' && value > 0 ? value : fallback;
}

void metric(const char* stage, uint64_t items, double elapsed) {
    std::cout << stage << ',' << items << ',' << std::fixed
              << std::setprecision(6) << elapsed << ','
              << (elapsed > 0 ? static_cast<double>(items) / elapsed : 0.0)
              << '\n';
}

}  // namespace

int main(int argc, char** argv) try {
    uint64_t point_count = 1000000;
    uint64_t grid_side = 4096;
    uint64_t edit_count = 128;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--points") point_count = parse_u64(argv[i + 1], point_count);
        else if (key == "--grid-side") grid_side = parse_u64(argv[i + 1], grid_side);
        else if (key == "--edits") edit_count = parse_u64(argv[i + 1], edit_count);
        else {
            std::cerr << "unknown option: " << key << '\n';
            return 2;
        }
    }
    point_count = std::max<uint64_t>(point_count, 3);
    grid_side = std::max<uint64_t>(grid_side, 2);

    std::cout << "stage,items,seconds,items_per_second\n";
    std::vector<dt_point3> points;
    points.reserve(static_cast<size_t>(point_count));
    std::mt19937_64 random(20260719ULL);
    std::uniform_real_distribution<double> xy(0.0, 100000.0);
    for (uint64_t i = 0; i < point_count; ++i) {
        const double x = xy(random) + i * 1e-9;
        const double y = xy(random) - i * 1e-9;
        points.push_back({x, y, 300.0 * std::sin(x * 0.00008) *
                                      std::cos(y * 0.00006)});
    }

    auto mesh = dterrain::make_mesh();
    auto begin = clock_type::now();
    dterrain::check(dt_build(mesh.get(), points.data(), points.size(), nullptr));
    metric("tin_build", point_count, seconds(begin));
    points.clear();
    points.shrink_to_fit();
    dterrain::check(dt_validate(mesh.get(), 0));

    begin = clock_type::now();
    for (uint64_t i = 0; i < edit_count; ++i) {
        const dt_point3 point{50000.0 + 0.001 * static_cast<double>(i + 1),
                              50000.0 - 0.0013 * static_cast<double>(i + 1),
                              static_cast<double>(i)};
        dt_vertex_id id = 0;
        dterrain::check(dt_insert_point(mesh.get(), &point, &id, nullptr));
        dterrain::check(dt_delete_vertex(mesh.get(), id, nullptr));
    }
    metric("tin_insert_delete_pairs", edit_count, seconds(begin));

    begin = clock_type::now();
    uint64_t query_triangles = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        const double c = 100.0 * static_cast<double>(i % 1000);
        const dt_bounds2 bounds{c, c, c + 250.0, c + 250.0};
        dt_query_result result = nullptr;
        dterrain::check(dt_query_triangles(mesh.get(), &bounds, &result));
        dterrain::query_result owned(result);
        dt_query_result_view view{};
        view.struct_size = sizeof(view);
        dterrain::check(dt_query_result_get_view(owned.get(), &view));
        query_triangles += view.triangle_count;
    }
    metric("tin_window_queries", 1000, seconds(begin));
    std::cerr << "queried_triangles=" << query_triangles << '\n';

    dt_grid_create_options create{};
    create.struct_size = sizeof(create);
    create.width = grid_side;
    create.height = grid_side;
    create.geo_transform[0] = 0;
    create.geo_transform[1] = 2;
    create.geo_transform[2] = 0;
    create.geo_transform[3] = 0;
    create.geo_transform[4] = 0;
    create.geo_transform[5] = 2;
    auto grid = dterrain::make_grid(create);
    std::vector<double> row(static_cast<size_t>(grid_side));
    begin = clock_type::now();
    for (uint64_t y = 0; y < grid_side; ++y) {
        for (uint64_t x = 0; x < grid_side; ++x) {
            row[static_cast<size_t>(x)] =
                50.0 * std::sin(x * 0.004) * std::cos(y * 0.003);
        }
        dterrain::check(dt_grid_write_window(grid.get(), 0, y, grid_side, 1,
                                              row.data(), grid_side));
    }
    metric("grid_stream_write", grid_side * grid_side, seconds(begin));

    std::vector<double> overview(1024 * 1024);
    dt_grid_overview_options overview_options{};
    overview_options.struct_size = sizeof(overview_options);
    overview_options.method = DT_GRID_OVERVIEW_AVERAGE;
    dt_grid_overview_result overview_result{};
    overview_result.struct_size = sizeof(overview_result);
    begin = clock_type::now();
    dterrain::check(dt_grid_read_overview(
        grid.get(), &overview_options, 1024, 1024, overview.data(), 1024,
        &overview_result));
    metric("grid_overview", grid_side * grid_side, seconds(begin));

    const auto path = std::filesystem::temp_directory_path() /
                      "dterrain_v1_stress.dgridb";
    begin = clock_type::now();
    dterrain::check(dt_grid_save_binary(grid.get(), path.string().c_str()));
    metric("grid_binary_save", grid_side * grid_side, seconds(begin));
    dt_grid_handle loaded_raw = nullptr;
    begin = clock_type::now();
    dterrain::check(dt_grid_load_binary(path.string().c_str(), &loaded_raw));
    dterrain::grid loaded(loaded_raw);
    metric("grid_binary_lazy_load", grid_side * grid_side, seconds(begin));
    dterrain::check(dt_grid_verify_binary_file(path.string().c_str()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    std::cerr << "estimated_raw_grid_mib="
              << (grid_side * grid_side * sizeof(double) / 1048576.0) << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "stress suite failed: " << error.what() << '\n';
    return 1;
}
