#include "dt_task_api.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kWidth = 1024;
constexpr uint64_t kHeight = 4096;
constexpr uint64_t kBlockBytes = 4ULL * 1024ULL * 1024ULL;

dt_grid_handle create_grid() {
    dt_grid_create_options options{};
    options.struct_size = sizeof(options);
    options.width = kWidth;
    options.height = kHeight;
    options.geo_transform[1] = 1.0;
    options.geo_transform[5] = -1.0;
    dt_grid_handle grid = nullptr;
    assert(dt_grid_create(&options, &grid) == DT_OK);
    std::vector<double> row(kWidth);
    for (uint64_t y = 0; y < kHeight; ++y) {
        for (uint64_t x = 0; x < kWidth; ++x) {
            row[static_cast<size_t>(x)] =
                static_cast<double>(y) * 0.25 + static_cast<double>(x) * 0.5;
        }
        assert(dt_grid_write_window(grid, 0, y, kWidth, 1, row.data(), 0) ==
               DT_OK);
    }
    return grid;
}

void wait_finished(dt_task_handle task, dt_task_info& info) {
    int32_t completed = 0;
    assert(dt_task_wait(task, UINT32_MAX, &completed) == DT_OK);
    assert(completed == 1);
    info = {};
    info.struct_size = sizeof(info);
    assert(dt_task_get_info(task, &info) == DT_OK);
}

uint64_t raw_offset(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::array<unsigned char, 65536> header{};
    stream.read(reinterpret_cast<char*>(header.data()), header.size());
    assert(stream);
    uint64_t result = 0;
    std::memcpy(&result, header.data() + 128, sizeof(result));
    return result;
}

} // namespace

int main() {
    static_assert(sizeof(dt_grid_verify_result) == 64);
    const std::filesystem::path file = "grid_window_verify.dgridb";
    const std::filesystem::path corrupt = "grid_window_verify_corrupt.dgridb";
    std::filesystem::remove(file);
    std::filesystem::remove(corrupt);

    dt_grid_handle source = create_grid();
    assert(dt_grid_verify_window(source, 0, 0, 1, 1, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_verify_result unsupported{};
    unsupported.struct_size = sizeof(unsupported);
    assert(dt_grid_verify_window(source, 0, 0, 1, 1, &unsupported) ==
           DT_E_UNSUPPORTED);
    assert(dt_grid_save_binary(source, file.string().c_str()) == DT_OK);
    dt_grid_destroy(source);

    dt_grid_handle mapped = nullptr;
    assert(dt_grid_load_binary(file.string().c_str(), &mapped) == DT_OK);
    dt_grid_verify_result first{};
    first.struct_size = sizeof(first);
    assert(dt_grid_verify_window(mapped, 10, 10, 32, 32, &first) == DT_OK);
    assert(first.block_count == 1);
    assert(first.verified_block_count == 1);
    assert(first.cached_block_count == 0);
    assert(first.checked_byte_count == kBlockBytes);
    assert(first.block_byte_size == kBlockBytes);

    dt_grid_verify_result cached{};
    cached.struct_size = sizeof(cached);
    assert(dt_grid_verify_window(mapped, 20, 20, 8, 8, &cached) == DT_OK);
    assert(cached.block_count == 1);
    assert(cached.verified_block_count == 0);
    assert(cached.cached_block_count == 1);
    assert((cached.flags & DT_GRID_VERIFY_USED_CACHE) != 0);

    dt_grid_verify_result crossing{};
    crossing.struct_size = sizeof(crossing);
    assert(dt_grid_verify_window(mapped, 0, 500, kWidth, 24, &crossing) ==
           DT_OK);
    assert(crossing.block_count == 2);
    assert(crossing.cached_block_count == 1);
    assert(crossing.verified_block_count == 1);

    dt_task_handle task = nullptr;
    assert(dt_grid_verify_window_async(mapped, 0, 1100, kWidth, 16, &task) ==
           DT_OK);
    dt_task_info info{};
    wait_finished(task, info);
    assert(info.state == DT_TASK_SUCCEEDED);
    assert(info.result_kind == DT_TASK_RESULT_GRID_VERIFICATION);
    assert(info.progress == 1.0);
    dt_grid_verify_result asynchronous{};
    asynchronous.struct_size = sizeof(asynchronous);
    assert(dt_task_get_grid_verification_result(task, &asynchronous) == DT_OK);
    assert(asynchronous.block_count == 1);
    assert(asynchronous.verified_block_count == 1);
    assert(dt_task_get_grid_verification_result(task, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_task_destroy(task);

    dt_grid_view_request_options view_request{};
    view_request.struct_size = sizeof(view_request);
    view_request.flags = DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE |
                         DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS |
                         DT_GRID_VIEW_REQUEST_USE_PYRAMID;
    view_request.world_bounds = {100.0, -2050.0, 900.0, -2000.0};
    view_request.output_width = 64;
    view_request.output_height = 16;
    view_request.overview_method = DT_GRID_OVERVIEW_AVERAGE;
    view_request.tile_row_count = 1;
    task = nullptr;
    assert(dt_grid_read_view_async(mapped, &view_request, &task) == DT_OK);
    wait_finished(task, info);
    assert(info.state == DT_TASK_SUCCEEDED);
    assert(info.result_kind == DT_TASK_RESULT_GRID_VIEW);
    assert(info.progress == 1.0);
    dt_grid_view_result view_result{};
    view_result.struct_size = sizeof(view_result);
    assert(dt_task_get_grid_view_result(task, &view_result) == DT_OK);
    assert((view_result.flags & DT_GRID_VIEW_RESULT_PREFETCH_REQUESTED) != 0);
    assert((view_result.flags & DT_GRID_VIEW_RESULT_SOURCE_VERIFIED) != 0);
    assert(view_result.width == 64 && view_result.height == 16);
    assert(view_result.values != nullptr);
    assert(view_result.source_window.column == 100);
    assert(view_result.source_window.row == 2000);
    assert(view_result.source_window.width == 801);
    assert(view_result.source_window.height == 51);
    assert(view_result.verification.block_count >= 1);
    assert((view_result.overview.flags & DT_GRID_OVERVIEW_USED_PYRAMID) != 0);
    assert(dt_task_get_grid_view_result(task, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_task_destroy(task);

    dt_grid_handle retained = nullptr;
    assert(dt_grid_load_binary(file.string().c_str(), &retained) == DT_OK);
    task = nullptr;
    assert(dt_grid_verify_window_async(retained, 0, 0, kWidth, kHeight,
                                       &task) == DT_OK);
    dt_grid_destroy(retained);
    assert(dt_task_request_cancel(task) == DT_OK);
    wait_finished(task, info);
    assert(info.state == DT_TASK_CANCELLED);
    assert(info.result_status == DT_E_CANCELLED);
    assert(dt_task_get_grid_verification_result(task, &asynchronous) ==
           DT_E_NOT_FOUND);
    dt_task_destroy(task);

    dt_grid_overview_options overview{};
    overview.struct_size = sizeof(overview);
    overview.method = DT_GRID_OVERVIEW_AVERAGE;
    overview.flags = DT_GRID_OVERVIEW_VERIFY_SOURCE_BLOCKS;
    overview.source_row = 500;
    overview.source_width = kWidth;
    overview.source_height = 24;
    std::vector<double> preview(64 * 16);
    assert(dt_grid_read_overview(mapped, &overview, 64, 12, preview.data(), 0,
                                 nullptr) == DT_OK);
    assert(dt_grid_verify_window(mapped, kWidth, 0, 1, 1, &first) ==
           DT_E_INVALID_ARGUMENT);
    assert(dt_grid_verify_window_async(nullptr, 0, 0, 1, 1, &task) ==
           DT_E_NOT_INITIALIZED);
    assert(dt_grid_verify_window_async(mapped, 0, 0, 1, 1, nullptr) ==
           DT_E_INVALID_ARGUMENT);
    dt_grid_destroy(mapped);

    std::filesystem::copy_file(
        file, corrupt, std::filesystem::copy_options::overwrite_existing);
    const uint64_t data_offset = raw_offset(corrupt);
    {
        std::fstream stream(corrupt,
                            std::ios::binary | std::ios::in | std::ios::out);
        const uint64_t damaged = data_offset + 6 * kBlockBytes + 64;
        stream.seekg(static_cast<std::streamoff>(damaged));
        char byte = 0;
        stream.read(&byte, 1);
        stream.clear();
        stream.seekp(static_cast<std::streamoff>(damaged));
        byte ^= 0x01;
        stream.write(&byte, 1);
        assert(stream);
    }

    dt_grid_handle damaged = nullptr;
    assert(dt_grid_load_binary(corrupt.string().c_str(), &damaged) == DT_OK);
    first = {};
    first.struct_size = sizeof(first);
    assert(dt_grid_verify_window(damaged, 0, 0, kWidth, 32, &first) == DT_OK);
    dt_grid_verify_result bad{};
    bad.struct_size = sizeof(bad);
    assert(dt_grid_verify_window(damaged, 0, 3100, kWidth, 32, &bad) ==
           DT_E_CORRUPTED_DATA);
    assert(dt_grid_verify_window(damaged, 0, 3100, kWidth, 32, &bad) ==
           DT_E_CORRUPTED_DATA);
    overview.source_row = 3100;
    overview.source_height = 32;
    assert(dt_grid_read_overview(damaged, &overview, 64, 16, preview.data(), 0,
                                 nullptr) == DT_E_CORRUPTED_DATA);
    task = nullptr;
    assert(dt_grid_read_overview_async(damaged, &overview, 64, 16, &task) ==
           DT_OK);
    wait_finished(task, info);
    assert(info.state == DT_TASK_FAILED);
    assert(info.result_status == DT_E_CORRUPTED_DATA);
    dt_grid_overview_view missing{};
    missing.struct_size = sizeof(missing);
    assert(dt_task_get_grid_overview_result(task, &missing) == DT_E_NOT_FOUND);
    dt_task_destroy(task);

    view_request.world_bounds = {100.0, -3132.0, 900.0, -3100.0};
    view_request.output_width = 64;
    view_request.output_height = 16;
    task = nullptr;
    assert(dt_grid_read_view_async(damaged, &view_request, &task) == DT_OK);
    wait_finished(task, info);
    assert(info.state == DT_TASK_FAILED);
    assert(info.result_status == DT_E_CORRUPTED_DATA);
    view_result = {};
    view_result.struct_size = sizeof(view_result);
    assert(dt_task_get_grid_view_result(task, &view_result) == DT_E_NOT_FOUND);
    dt_task_destroy(task);
    dt_grid_destroy(damaged);

    std::filesystem::remove(file);
    std::filesystem::remove(corrupt);
    return 0;
}
