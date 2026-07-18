#include "dt_terrain_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kWidth = 700;
constexpr uint64_t kHeight = 300;
constexpr double kNoData = -9999.0;

double value_at(uint64_t column, uint64_t row) {
    if ((column + row * 3) % 97 == 0) return kNoData;
    return 100.0 + static_cast<double>(column) * 0.25 -
           static_cast<double>(row) * 0.5;
}

dt_grid_handle create_grid() {
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    options.flags = DT_GRID_HAS_NODATA;
    options.width = kWidth;
    options.height = kHeight;
    options.geo_transform[0] = 500000.0;
    options.geo_transform[1] = 2.0;
    options.geo_transform[2] = 0.25;
    options.geo_transform[3] = 3200000.0;
    options.geo_transform[4] = -0.15;
    options.geo_transform[5] = -2.0;
    options.nodata_value = kNoData;
    dt_grid_handle grid = nullptr;
    assert(dt_grid_create(&options, &grid) == DT_OK);
    std::vector<double> row(kWidth);
    for (uint64_t y = 0; y < kHeight; ++y) {
        for (uint64_t x = 0; x < kWidth; ++x) row[x] = value_at(x, y);
        assert(dt_grid_write_window(grid, 0, y, kWidth, 1, row.data(), 0) ==
               DT_OK);
    }
    assert(dt_grid_set_crs_wkt(grid,
        "PROJCS[\"Local test grid\",UNIT[\"metre\",1]]") == DT_OK);
    return grid;
}

void assert_value(dt_grid_handle grid, uint64_t column, uint64_t row,
                  double expected) {
    double value = 0.0;
    assert(dt_grid_read_window(grid, column, row, 1, 1, &value, 0) == DT_OK);
    assert(value == expected);
}

} // namespace

int main() {
    const std::filesystem::path file = "grid_binary_roundtrip.dgridb";
    const std::filesystem::path modified_file =
        "grid_binary_modified.dgridb";
    const std::filesystem::path corrupt_file = "grid_binary_corrupt.dgridb";
    const std::filesystem::path checksum_file =
        "grid_binary_bad_checksum.dgridb";
    const std::filesystem::path truncated_file =
        "grid_binary_truncated.dgridb";
    std::filesystem::remove(file);
    std::filesystem::remove(modified_file);
    std::filesystem::remove(corrupt_file);
    std::filesystem::remove(checksum_file);
    std::filesystem::remove(truncated_file);

    assert(dt_grid_save_binary(nullptr, file.string().c_str()) ==
           DT_E_NOT_INITIALIZED);
    assert(dt_grid_load_binary(nullptr, nullptr) == DT_E_INVALID_ARGUMENT);
    dt_grid_create_options invalid_options{};
    invalid_options.struct_size = sizeof(invalid_options);
    invalid_options.flags = DT_GRID_STORAGE_MEMORY_MAPPED;
    invalid_options.width = 2;
    invalid_options.height = 2;
    invalid_options.geo_transform[1] = 1.0;
    invalid_options.geo_transform[5] = -1.0;
    dt_grid_handle invalid_creation = nullptr;
    assert(dt_grid_create(&invalid_options, &invalid_creation) ==
           DT_E_INVALID_ARGUMENT);
    assert(invalid_creation == nullptr);

    dt_grid_handle source = create_grid();
    std::vector<double> expected_overview(512 * kHeight);
    dt_grid_overview_options overview_options{};
    overview_options.struct_size = sizeof(overview_options);
    overview_options.method = DT_GRID_OVERVIEW_AVERAGE;
    dt_grid_overview_result expected_result{};
    expected_result.struct_size = sizeof(expected_result);
    assert(dt_grid_read_overview(source, &overview_options, 512, kHeight,
                                 expected_overview.data(), 0,
                                 &expected_result) == DT_OK);
    assert(dt_grid_save_binary(source, file.string().c_str()) == DT_OK);
    assert(std::filesystem::file_size(file) > kWidth * kHeight * sizeof(double));

    dt_grid_handle mapped = nullptr;
    assert(dt_grid_load_binary(file.string().c_str(), &mapped) == DT_OK);
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    assert(dt_grid_get_info(mapped, &info) == DT_OK);
    assert(info.width == kWidth && info.height == kHeight);
    assert((info.flags & DT_GRID_HAS_NODATA) != 0);
    assert((info.flags & DT_GRID_HAS_PERSISTENT_OVERVIEW) != 0);
#ifdef _WIN32
    assert((info.flags & DT_GRID_STORAGE_MEMORY_MAPPED) != 0);
#endif
    assert(info.valid_value_count == expected_result.valid_value_count);

    size_t crs_size = 0;
    assert(dt_grid_get_crs_wkt(mapped, nullptr, 0, &crs_size) == DT_OK);
    std::vector<char> crs(crs_size);
    assert(dt_grid_get_crs_wkt(mapped, crs.data(), crs.size(), nullptr) ==
           DT_OK);
    assert(std::string(crs.data()) ==
           "PROJCS[\"Local test grid\",UNIT[\"metre\",1]]");
    assert_value(mapped, 321, 123, value_at(321, 123));

    std::vector<double> actual_overview(512 * kHeight);
    dt_grid_overview_result actual_result{};
    actual_result.struct_size = sizeof(actual_result);
    assert(dt_grid_read_overview(mapped, &overview_options, 512, kHeight,
                                 actual_overview.data(), 0,
                                 &actual_result) == DT_OK);
    assert(actual_overview == expected_overview);
    assert(actual_result.valid_value_count == expected_result.valid_value_count);
    assert(actual_result.nodata_value_count ==
           expected_result.nodata_value_count);
    assert(actual_result.minimum_value == expected_result.minimum_value);
    assert(actual_result.maximum_value == expected_result.maximum_value);
    assert(actual_result.mean_value == expected_result.mean_value);

    const double changed = 9876.5;
    assert(dt_grid_write_window(mapped, 321, 123, 1, 1, &changed, 0) == DT_OK);
    assert_value(mapped, 321, 123, changed);
    info = {};
    info.struct_size = sizeof(info);
    assert(dt_grid_get_info(mapped, &info) == DT_OK);
    assert((info.flags & DT_GRID_HAS_PERSISTENT_OVERVIEW) == 0);
    assert(info.valid_value_count == expected_result.valid_value_count);

    assert(value_at(322, 123) != kNoData);
    assert(dt_grid_write_window(mapped, 322, 123, 1, 1, &kNoData, 0) == DT_OK);
    info = {};
    info.struct_size = sizeof(info);
    assert(dt_grid_get_info(mapped, &info) == DT_OK);
    assert(info.valid_value_count + 1 == expected_result.valid_value_count);

    dt_grid_handle original_again = nullptr;
    assert(dt_grid_load_binary(file.string().c_str(), &original_again) == DT_OK);
    assert_value(original_again, 321, 123, value_at(321, 123));
    dt_grid_destroy(original_again);

    // Explicitly saving back to the mapped source atomically commits the
    // private copy-on-write view; the live handle remains valid afterwards.
    assert(dt_grid_save_binary(mapped, file.string().c_str()) == DT_OK);
    dt_grid_handle committed = nullptr;
    assert(dt_grid_load_binary(file.string().c_str(), &committed) == DT_OK);
    assert_value(committed, 321, 123, changed);
    assert_value(committed, 322, 123, kNoData);
    info = {};
    info.struct_size = sizeof(info);
    assert(dt_grid_get_info(committed, &info) == DT_OK);
    assert(info.valid_value_count + 1 == expected_result.valid_value_count);
    dt_grid_destroy(committed);

    assert(dt_grid_save_binary(mapped, modified_file.string().c_str()) == DT_OK);
    dt_grid_handle saved_change = nullptr;
    assert(dt_grid_load_binary(modified_file.string().c_str(), &saved_change) ==
           DT_OK);
    assert_value(saved_change, 321, 123, changed);
    dt_grid_destroy(saved_change);

    std::filesystem::copy_file(file, corrupt_file,
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream stream(corrupt_file,
                            std::ios::binary | std::ios::in | std::ios::out);
        char bad = 'X';
        stream.write(&bad, 1);
    }
    dt_grid_handle invalid = reinterpret_cast<dt_grid_handle>(uintptr_t{1});
    assert(dt_grid_load_binary(corrupt_file.string().c_str(), &invalid) ==
           DT_E_CORRUPTED_DATA);
    assert(invalid == nullptr);

    std::filesystem::copy_file(file, checksum_file,
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream stream(checksum_file,
                            std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(300);
        char byte = 0;
        stream.read(&byte, 1);
        stream.clear();
        stream.seekp(300);
        byte ^= 0x01;
        stream.write(&byte, 1);
    }
    assert(dt_grid_load_binary(checksum_file.string().c_str(), &invalid) ==
           DT_E_CORRUPTED_DATA);
    assert(invalid == nullptr);

    std::filesystem::copy_file(file, truncated_file,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(truncated_file, 65536 + 16);
    assert(dt_grid_load_binary(truncated_file.string().c_str(), &invalid) ==
           DT_E_CORRUPTED_DATA);
    assert(invalid == nullptr);

    dt_grid_destroy(mapped);
    dt_grid_destroy(source);
    std::filesystem::remove(file);
    std::filesystem::remove(modified_file);
    std::filesystem::remove(corrupt_file);
    std::filesystem::remove(checksum_file);
    std::filesystem::remove(truncated_file);
    return 0;
}
