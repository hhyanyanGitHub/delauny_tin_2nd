#include "dt_grid_storage.hpp"

#include "dt_core.hpp"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dt {

struct GridStorage::Mapping {
    double* data = nullptr;
    size_t count = 0;
    std::filesystem::path path;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;

    ~Mapping() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
#else
    std::vector<double> fallback;
#endif
};

GridStorage::GridStorage() = default;
GridStorage::~GridStorage() = default;

void GridStorage::assign(size_t count, double value) {
    mapping_.reset();
    owned_.assign(count, value);
}

void GridStorage::adopt(std::vector<double>&& values) {
    mapping_.reset();
    owned_ = std::move(values);
}

void GridStorage::map_copy_on_write(const std::filesystem::path& file,
                                    size_t byte_offset, size_t count) {
    if (count > std::numeric_limits<size_t>::max() / sizeof(double)) {
        throw Exception(DT_E_LIMIT_EXCEEDED,
                        "binary GRID mapping is too large");
    }
    auto result = std::make_unique<Mapping>();
    result->path = file;
#ifdef _WIN32
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    if (byte_offset % system_info.dwAllocationGranularity != 0) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "binary GRID data offset is not mapping aligned");
    }
    result->file = CreateFileW(
        file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (result->file == INVALID_HANDLE_VALUE) {
        throw Exception(DT_E_IO, "cannot open binary GRID for mapping");
    }
    result->mapping = CreateFileMappingW(result->file, nullptr, PAGE_WRITECOPY,
                                         0, 0, nullptr);
    if (!result->mapping) {
        throw Exception(DT_E_IO, "cannot create binary GRID file mapping");
    }
    const uint64_t offset = static_cast<uint64_t>(byte_offset);
    const size_t bytes = count * sizeof(double);
    result->data = static_cast<double*>(MapViewOfFile(
        result->mapping, FILE_MAP_COPY, static_cast<DWORD>(offset >> 32U),
        static_cast<DWORD>(offset & 0xffffffffU), bytes));
    if (!result->data) {
        throw Exception(DT_E_IO, "cannot map binary GRID value array");
    }
    result->count = count;
#else
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw Exception(DT_E_IO, "cannot open binary GRID");
    stream.seekg(static_cast<std::streamoff>(byte_offset));
    result->fallback.resize(count);
    stream.read(reinterpret_cast<char*>(result->fallback.data()),
                static_cast<std::streamsize>(count * sizeof(double)));
    if (!stream) throw Exception(DT_E_IO, "cannot read binary GRID values");
    result->data = result->fallback.data();
    result->count = count;
#endif
    owned_.clear();
    owned_.shrink_to_fit();
    mapping_ = std::move(result);
}

bool GridStorage::maps_file(const std::filesystem::path& file) const {
    if (!mapping_) return false;
    std::error_code error;
    if (std::filesystem::equivalent(mapping_->path, file, error)) return true;
    error.clear();
    const auto mapped = std::filesystem::absolute(mapping_->path, error);
    if (error) return false;
    const auto requested = std::filesystem::absolute(file, error);
    if (error) return false;
#ifdef _WIN32
    std::wstring left = mapped.lexically_normal().wstring();
    std::wstring right = requested.lexically_normal().wstring();
    std::transform(left.begin(), left.end(), left.begin(), ::towlower);
    std::transform(right.begin(), right.end(), right.begin(), ::towlower);
    return left == right;
#else
    return mapped.lexically_normal() == requested.lexically_normal();
#endif
}

void GridStorage::materialize() {
    if (!mapping_) return;
    std::vector<double> copy(mapping_->data,
                             mapping_->data + mapping_->count);
    mapping_.reset();
    owned_ = std::move(copy);
}

void GridStorage::prefetch(size_t first, size_t count) const noexcept {
#ifdef _WIN32
    if (!mapping_ || count == 0 || first >= mapping_->count ||
        count > mapping_->count - first) {
        return;
    }
    using PrefetchVirtualMemoryFunction = BOOL(WINAPI*)(
        HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    const FARPROC address = GetProcAddress(kernel, "PrefetchVirtualMemory");
    PrefetchVirtualMemoryFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    if (!function) return;
    WIN32_MEMORY_RANGE_ENTRY range{};
    range.VirtualAddress = mapping_->data + first;
    range.NumberOfBytes = count * sizeof(double);
    function(GetCurrentProcess(), 1, &range, 0);
#else
    (void)first;
    (void)count;
#endif
}

bool GridStorage::is_mapped() const noexcept {
#ifdef _WIN32
    return mapping_ != nullptr;
#else
    return false;
#endif
}

size_t GridStorage::size() const noexcept {
    return mapping_ ? mapping_->count : owned_.size();
}

double* GridStorage::data() noexcept {
    return mapping_ ? mapping_->data : owned_.data();
}

const double* GridStorage::data() const noexcept {
    return mapping_ ? mapping_->data : owned_.data();
}

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw Exception(DT_E_IO, "cannot replace binary GRID destination");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw Exception(DT_E_IO, "cannot replace binary GRID destination");
    }
#endif
}

} // namespace dt
