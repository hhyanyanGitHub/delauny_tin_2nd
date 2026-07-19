#include "dt_cdt_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require_ok(dt_status status, const char* operation) {
    if (status == DT_OK) return;
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
    std::cerr << operation << " failed: " << status << " " << error << "\n";
    std::abort();
}

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "requirement failed: " << message << "\n";
    std::abort();
}

std::filesystem::path temporary_path(const char* suffix) {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("dterrain_cdt_v070_" + std::to_string(stamp) + suffix);
}

} // namespace

int main() {
    dt_cdt_options options{};
    options.struct_size = sizeof(options);
    options.crossing_policy = DT_CDT_CROSSING_SPLIT_COMPATIBLE_Z;
    options.crossing_z_tolerance = 1e-8;
    dt_cdt_handle cdt = nullptr;
    require_ok(dt_cdt_create(&options, &cdt), "create split-policy CDT");

    const dt_point3 base[] = {{0, 0, 0}, {10, 0, 0},
                              {10, 10, 10}, {0, 10, 10}};
    require_ok(dt_cdt_build(cdt, base, 4), "build CDT");
    const dt_point3 diagonal_a[] = {{0, 0, 0}, {10, 10, 10}};
    const dt_point3 diagonal_b[] = {{0, 10, 10}, {10, 0, 0}};
    dt_constraint_id first = 0;
    dt_constraint_id second = 0;
    require_ok(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                                     diagonal_a, 2, &first),
               "add first diagonal");
    require_ok(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                                     diagonal_b, 2, &second),
               "auto-node crossing diagonal");

    dt_constraint_info info{};
    require_ok(dt_cdt_get_constraint_info(cdt, 0, &info),
               "read noded constraint info");
    require(info.id == first && info.point_count == 3,
            "existing line was materialized with its crossing vertex");
    std::vector<dt_point3> first_points(3);
    uint64_t required_points = 0;
    require_ok(dt_cdt_copy_constraint_points(cdt, first, first_points.data(),
                                             first_points.size(),
                                             &required_points),
               "copy noded constraint");
    require(required_points == 3 && first_points[1].x == 5.0 &&
                first_points[1].y == 5.0 && first_points[1].z == 5.0,
            "crossing XYZ was interpolated consistently");

    dt_cdt_statistics before_rejected{};
    require_ok(dt_cdt_get_statistics(cdt, &before_rejected),
               "statistics before rejected crossing");
    const dt_point3 incompatible[] = {{5, 0, 100}, {5, 10, 100}};
    dt_constraint_id rejected_id = 99;
    require(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                                  incompatible, 2, &rejected_id) ==
                DT_E_INVALID_ARGUMENT && rejected_id == 0,
            "incompatible crossing Z is rejected");
    dt_cdt_statistics after_rejected{};
    require_ok(dt_cdt_get_statistics(cdt, &after_rejected),
               "statistics after rejected crossing");
    require(after_rejected.generation == before_rejected.generation &&
                after_rejected.constraint_count ==
                    before_rejected.constraint_count,
            "rejected auto-node edit is atomic");

    require_ok(dt_cdt_set_crossing_policy(cdt, DT_CDT_CROSSING_REJECT, 0.0),
               "switch to reject policy");
    const dt_point3 local_line[] = {{0, 1, 1}, {2, 3, 3}};
    dt_constraint_id local_id = 0;
    require_ok(dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                                     local_line, 2, &local_id),
               "locally insert non-crossing breakline");
    dt_cdt_edit_metrics metrics{};
    require_ok(dt_cdt_get_edit_metrics(cdt, &metrics), "read edit metrics");
    require(metrics.last_edit_mode == DT_CDT_EDIT_MODE_LOCAL_TOPOLOGY &&
                metrics.local_topology_edit_count == 1 &&
                metrics.full_rebuild_count >= 3,
            "local topology path was reported");
    require_ok(dt_cdt_validate(cdt, 0), "validate locally edited CDT");

    const auto binary = temporary_path(".dcdtb");
    const auto corrupted = temporary_path("_corrupt.dcdtb");
    const auto binary_utf8 = binary.u8string();
    require_ok(dt_cdt_set_crs_wkt(cdt, "LOCAL_CS[\"v070\"]"), "set CRS");
    require_ok(dt_cdt_save_binary(cdt, binary_utf8.c_str()), "save DCDTB");
    require_ok(dt_cdt_verify_binary_file(binary_utf8.c_str()), "verify DCDTB");

    const dt_bounds2 center{4.9, 4.9, 5.1, 5.1};
    uint64_t entry_count = 0;
    require_ok(dt_cdt_query_binary_index(binary_utf8.c_str(), &center, nullptr,
                                         0, &entry_count),
               "count binary window entries");
    require(entry_count == 2, "center window finds the crossing diagonals");
    std::vector<dt_cdt_binary_index_entry> entries(entry_count);
    require_ok(dt_cdt_query_binary_index(binary_utf8.c_str(), &center,
                                         entries.data(), entries.size(),
                                         &entry_count),
               "read binary window entries");
    require(std::all_of(entries.begin(), entries.end(),
                        [](const auto& entry) {
                            return entry.struct_size == sizeof(entry) &&
                                   entry.point_count == 3;
                        }),
            "binary directory contains noded line metadata");

    dt_constraint_info binary_info{};
    required_points = 0;
    require_ok(dt_cdt_read_binary_constraint(binary_utf8.c_str(), first,
                                             nullptr, 0, &required_points,
                                             &binary_info),
               "inspect binary constraint");
    require(required_points == 3 && binary_info.id == first,
            "binary direct constraint lookup works");

    dt_cdt_handle loaded = nullptr;
    require_ok(dt_cdt_create(nullptr, &loaded), "create load destination");
    dt_bounds2 loaded_bounds{};
    require_ok(dt_cdt_load_binary(loaded, binary_utf8.c_str(), &loaded_bounds),
               "load DCDTB");
    dt_cdt_statistics original_stats{};
    dt_cdt_statistics loaded_stats{};
    require_ok(dt_cdt_get_statistics(cdt, &original_stats), "source stats");
    require_ok(dt_cdt_get_statistics(loaded, &loaded_stats), "loaded stats");
    require(original_stats.constraint_count == loaded_stats.constraint_count &&
                original_stats.domain_triangle_count ==
                    loaded_stats.domain_triangle_count,
            "binary roundtrip preserves CDT topology statistics");
    require_ok(dt_cdt_validate(loaded, 0), "validate loaded DCDTB");

    std::filesystem::copy_file(binary, corrupted,
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream file(corrupted, std::ios::binary | std::ios::in |
                                        std::ios::out);
        file.seekg(-1, std::ios::end);
        char value = 0;
        file.read(&value, 1);
        value ^= 0x5a;
        file.seekp(-1, std::ios::end);
        file.write(&value, 1);
    }
    require(dt_cdt_verify_binary_file(corrupted.u8string().c_str()) ==
                DT_E_CORRUPTED_DATA,
            "binary payload corruption is detected");

    dt_cdt_destroy(loaded);
    dt_cdt_destroy(cdt);
    std::filesystem::remove(binary);
    std::filesystem::remove(corrupted);
    std::cout << "v0.70 CDT tests passed\n";
    return 0;
}
