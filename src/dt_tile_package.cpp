#include "dt_tile_package.hpp"

#include "dt_grid_storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace dt {
namespace {

constexpr uint64_t kDefaultMaximumFileBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumFileBytes = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr size_t kHeaderSize = 4096;
constexpr size_t kRecordHeaderSize = 128;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEndianMarker = 0x01020304U;
constexpr char kMagic[8] = {'D', 'G', 'T', 'I', 'L', 'E', '1', '\0'};
constexpr char kRecordMagic[8] = {'D', 'G', 'T', 'R', 'E', 'C', '1', '\0'};
constexpr size_t kHeaderChecksumOffset = 128;
constexpr size_t kRecordChecksumOffset = 120;

uint64_t hash_bytes(const void* data, size_t size,
                    uint64_t hash = 1469598103934665603ULL) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <class T>
void store(void* destination, size_t capacity, size_t offset,
           const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > capacity || sizeof(T) > capacity - offset) {
        throw Exception(DT_E_INTERNAL, "DGTILE header write overflow");
    }
    std::memcpy(static_cast<unsigned char*>(destination) + offset, &value,
                sizeof(T));
}

template <class T>
T load_value(const void* source, size_t capacity, size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > capacity || sizeof(T) > capacity - offset) {
        throw Exception(DT_E_CORRUPTED_DATA, "DGTILE header is truncated");
    }
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(source) + offset,
                sizeof(T));
    return value;
}

void read_exact(std::istream& stream, void* output, uint64_t bytes,
                const char* message) {
    auto* cursor = static_cast<char*>(output);
    constexpr uint64_t chunk_limit = 64ULL * 1024ULL * 1024ULL;
    while (bytes != 0) {
        const uint64_t chunk = std::min(bytes, chunk_limit);
        stream.read(cursor, static_cast<std::streamsize>(chunk));
        if (!stream) throw Exception(DT_E_CORRUPTED_DATA, message);
        cursor += static_cast<size_t>(chunk);
        bytes -= chunk;
    }
}

void write_exact(std::ostream& stream, const void* input, uint64_t bytes,
                 const char* message) {
    const auto* cursor = static_cast<const char*>(input);
    constexpr uint64_t chunk_limit = 64ULL * 1024ULL * 1024ULL;
    while (bytes != 0) {
        const uint64_t chunk = std::min(bytes, chunk_limit);
        stream.write(cursor, static_cast<std::streamsize>(chunk));
        if (!stream) throw Exception(DT_E_IO, message);
        cursor += static_cast<size_t>(chunk);
        bytes -= chunk;
    }
}

struct PersistentTileKeyHash {
    size_t operator()(const PersistentTileKey& key) const noexcept {
        size_t value = static_cast<size_t>(key.scale);
        const auto mix = [&](uint64_t item) {
            value ^= static_cast<size_t>(item) +
                     static_cast<size_t>(0x9e3779b97f4a7c15ULL) +
                     (value << 6U) + (value >> 2U);
        };
        mix(key.x);
        mix(key.y);
        mix(key.method);
        mix(key.flags);
        return value;
    }
};

std::filesystem::path required_package_path(const char* utf8_file_name) {
    if (!utf8_file_name || utf8_file_name[0] == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "DGTILE file name is empty");
    }
    return std::filesystem::u8path(utf8_file_name);
}

std::mutex g_writer_registry_mutex;
std::unordered_set<std::filesystem::path> g_writer_registry;

std::filesystem::path normalized_registry_path(
    const std::filesystem::path& package_path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(package_path, error);
    if (error) {
        error.clear();
        normalized = std::filesystem::absolute(package_path, error);
    }
    if (error) {
        throw Exception(DT_E_IO, "cannot normalize DGTILE file name");
    }
    normalized = normalized.lexically_normal();
#ifdef _WIN32
    auto native = normalized.native();
    std::transform(native.begin(), native.end(), native.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    return std::filesystem::path(std::move(native));
#else
    return normalized;
#endif
}

class PackageWriterLease final {
public:
    explicit PackageWriterLease(std::filesystem::path package_path)
        : path_(normalized_registry_path(package_path)) {
        std::lock_guard<std::mutex> lock(g_writer_registry_mutex);
        if (!g_writer_registry.emplace(path_).second) {
            throw Exception(DT_E_IO,
                            "DGTILE package already has a writable handle");
        }
    }

    ~PackageWriterLease() {
        std::lock_guard<std::mutex> lock(g_writer_registry_mutex);
        g_writer_registry.erase(path_);
    }

private:
    std::filesystem::path path_;
};

} // namespace

uint64_t grid_tile_source_fingerprint(const Grid& source,
                                      uint64_t source_revision) {
    uint64_t hash = 1469598103934665603ULL;
    const auto add = [&](const auto& value) {
        hash = hash_bytes(&value, sizeof(value), hash);
    };
    add(source_revision);
    const uint64_t width = source.width();
    const uint64_t height = source.height();
    add(width);
    add(height);
    const uint32_t semantic_flags = source.flags() & DT_GRID_HAS_NODATA;
    add(semantic_flags);
    for (size_t index = 0; index < 6; ++index) add(source.transform()[index]);
    add(source.nodata());
    const auto& crs = source.crs_wkt();
    hash = hash_bytes(crs.data(), crs.size(), hash);

    const uint64_t sample_columns = std::min<uint64_t>(64, width);
    const uint64_t sample_rows = std::min<uint64_t>(64, height);
    for (uint64_t row = 0; row < sample_rows; ++row) {
        const uint64_t y = sample_rows == 1 ? 0 : static_cast<uint64_t>(
            static_cast<long double>(row) * (height - 1) /
            static_cast<long double>(sample_rows - 1));
        for (uint64_t column = 0; column < sample_columns; ++column) {
            const uint64_t x = sample_columns == 1 ? 0 : static_cast<uint64_t>(
                static_cast<long double>(column) * (width - 1) /
                static_cast<long double>(sample_columns - 1));
            add(source.values()[static_cast<size_t>(y * width + x)]);
        }
    }
    return hash;
}

struct GridTilePackage::Impl {
    std::filesystem::path path;
    uint32_t tile_width = 0;
    uint32_t tile_height = 0;
    uint64_t source_width = 0;
    uint64_t source_height = 0;
    uint64_t source_fingerprint = 0;
    uint64_t maximum_file_bytes = kDefaultMaximumFileBytes;
    uint64_t file_bytes = kHeaderSize;
    uint64_t record_count = 0;
    uint64_t disk_hit_count = 0;
    uint64_t written_count = 0;
    uint64_t skipped_write_count = 0;
    bool read_only = false;
    bool reset_corrupted = false;
    bool write_disabled = false;
    std::array<unsigned char, kHeaderSize> header{};
    std::fstream writer;
    std::unique_ptr<PackageWriterLease> writer_lease;
    std::unordered_map<PersistentTileKey, PersistentTileDescriptor,
                       PersistentTileKeyHash> index;
    mutable std::mutex mutex;

    Impl(const Grid& source, uint32_t tile_width_value,
         uint32_t tile_height_value,
         const dt_grid_view_disk_cache_options& options)
        : path(required_package_path(options.utf8_file_name)),
          tile_width(tile_width_value), tile_height(tile_height_value),
          source_width(source.width()), source_height(source.height()),
          source_fingerprint(grid_tile_source_fingerprint(
              source, options.source_revision)),
          maximum_file_bytes(options.maximum_file_bytes == 0
              ? kDefaultMaximumFileBytes : options.maximum_file_bytes),
          read_only((options.flags & DT_GRID_VIEW_DISK_CACHE_READ_ONLY) != 0),
          reset_corrupted((options.flags &
              DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED) != 0) {
        constexpr uint32_t known_flags =
            DT_GRID_VIEW_DISK_CACHE_READ_ONLY |
            DT_GRID_VIEW_DISK_CACHE_RESET_STALE |
            DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
        if ((options.flags & ~known_flags) != 0) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "unknown DGTILE cache flags");
        }
        if (maximum_file_bytes < kHeaderSize + kRecordHeaderSize ||
            maximum_file_bytes > kMaximumFileBytes) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "DGTILE maximum file size is invalid");
        }
        if (!read_only)
            writer_lease = std::make_unique<PackageWriterLease>(path);

        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) throw Exception(DT_E_IO, "cannot inspect DGTILE file");
        if (!exists) {
            if (read_only) {
                throw Exception(DT_E_NOT_FOUND,
                                "read-only DGTILE file does not exist");
            }
            create_empty();
            open_writer();
            return;
        }

        try {
            load_existing();
        } catch (const Exception& exception) {
            const bool reset_stale = exception.status() == DT_E_STALE_QUERY &&
                (options.flags & DT_GRID_VIEW_DISK_CACHE_RESET_STALE) != 0;
            const bool reset_corrupted =
                exception.status() == DT_E_CORRUPTED_DATA &&
                (options.flags & DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED) != 0;
            if (read_only || (!reset_stale && !reset_corrupted)) throw;
            create_empty();
        }
        open_writer();
    }

    void open_writer() {
        if (read_only) return;
        writer.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!writer) {
            throw Exception(DT_E_IO, "cannot open DGTILE file for updates");
        }
    }

    std::array<unsigned char, kHeaderSize> make_header(
        uint64_t records, uint64_t declared_size) const {
        std::array<unsigned char, kHeaderSize> result{};
        std::memcpy(result.data(), kMagic, sizeof(kMagic));
        store(result.data(), result.size(), 8, kVersion);
        store(result.data(), result.size(), 12,
              static_cast<uint32_t>(kHeaderSize));
        store(result.data(), result.size(), 16, kEndianMarker);
        store(result.data(), result.size(), 20, uint32_t{0});
        store(result.data(), result.size(), 24, tile_width);
        store(result.data(), result.size(), 28, tile_height);
        store(result.data(), result.size(), 32, source_width);
        store(result.data(), result.size(), 40, source_height);
        store(result.data(), result.size(), 104, source_fingerprint);
        store(result.data(), result.size(), 112, records);
        store(result.data(), result.size(), 120, declared_size);
        store(result.data(), result.size(), kHeaderChecksumOffset, uint64_t{0});
        store(result.data(), result.size(), kHeaderChecksumOffset,
              hash_bytes(result.data(), result.size()));
        return result;
    }

    void create_empty() {
        index.clear();
        file_bytes = kHeaderSize;
        record_count = 0;
        header = make_header(0, file_bytes);
        auto temporary = path;
        temporary += "." + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".tmp";
        try {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw Exception(DT_E_IO, "cannot create DGTILE file");
            write_exact(stream, header.data(), header.size(),
                        "cannot write DGTILE header");
            stream.flush();
            if (!stream) throw Exception(DT_E_IO, "cannot flush DGTILE file");
            stream.close();
            replace_file_atomically(temporary, path);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    void load_existing() {
        std::error_code error;
        const uint64_t actual_size = std::filesystem::file_size(path, error);
        if (error || actual_size < kHeaderSize) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE file is smaller than its header");
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw Exception(DT_E_IO, "cannot open DGTILE file");
        read_exact(stream, header.data(), header.size(),
                   "cannot read DGTILE header");
        if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0 ||
            load_value<uint32_t>(header.data(), header.size(), 8) != kVersion ||
            load_value<uint32_t>(header.data(), header.size(), 12) != kHeaderSize ||
            load_value<uint32_t>(header.data(), header.size(), 16) != kEndianMarker) {
            throw Exception(DT_E_CORRUPTED_DATA, "invalid DGTILE header");
        }
        const uint64_t stored_checksum = load_value<uint64_t>(
            header.data(), header.size(), kHeaderChecksumOffset);
        store(header.data(), header.size(), kHeaderChecksumOffset, uint64_t{0});
        const uint64_t computed_checksum = hash_bytes(header.data(), header.size());
        store(header.data(), header.size(), kHeaderChecksumOffset,
              stored_checksum);
        if (stored_checksum != computed_checksum) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE header checksum mismatch");
        }
        if (load_value<uint32_t>(header.data(), header.size(), 24) != tile_width ||
            load_value<uint32_t>(header.data(), header.size(), 28) != tile_height ||
            load_value<uint64_t>(header.data(), header.size(), 32) != source_width ||
            load_value<uint64_t>(header.data(), header.size(), 40) != source_height ||
            load_value<uint64_t>(header.data(), header.size(), 104) !=
                source_fingerprint) {
            throw Exception(DT_E_STALE_QUERY,
                            "DGTILE package belongs to another GRID revision");
        }
        record_count = load_value<uint64_t>(header.data(), header.size(), 112);
        file_bytes = load_value<uint64_t>(header.data(), header.size(), 120);
        if (file_bytes < kHeaderSize || file_bytes > actual_size) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE declared file size is invalid");
        }
        if (record_count > (file_bytes - kHeaderSize) / kRecordHeaderSize) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE record count exceeds file capacity");
        }
        if (file_bytes >= maximum_file_bytes) write_disabled = true;

        uint64_t cursor = kHeaderSize;
        for (uint64_t record_index = 0; record_index < record_count;
             ++record_index) {
            if (cursor > file_bytes ||
                kRecordHeaderSize > file_bytes - cursor) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "DGTILE record directory is truncated");
            }
            std::array<unsigned char, kRecordHeaderSize> record{};
            stream.seekg(static_cast<std::streamoff>(cursor));
            if (!stream) throw Exception(DT_E_CORRUPTED_DATA,
                                         "DGTILE record offset is invalid");
            read_exact(stream, record.data(), record.size(),
                       "cannot read DGTILE record header");
            if (std::memcmp(record.data(), kRecordMagic,
                            sizeof(kRecordMagic)) != 0 ||
                load_value<uint32_t>(record.data(), record.size(), 8) !=
                    kRecordHeaderSize) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "invalid DGTILE record header");
            }
            const uint64_t stored_record_checksum = load_value<uint64_t>(
                record.data(), record.size(), kRecordChecksumOffset);
            store(record.data(), record.size(), kRecordChecksumOffset,
                  uint64_t{0});
            const uint64_t computed_record_checksum =
                hash_bytes(record.data(), record.size());
            if (stored_record_checksum != computed_record_checksum) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "DGTILE record header checksum mismatch");
            }
            PersistentTileDescriptor descriptor{};
            descriptor.key.scale = load_value<uint64_t>(record.data(), record.size(), 16);
            descriptor.key.x = load_value<uint64_t>(record.data(), record.size(), 24);
            descriptor.key.y = load_value<uint64_t>(record.data(), record.size(), 32);
            descriptor.key.method = load_value<uint32_t>(record.data(), record.size(), 40);
            descriptor.key.flags = load_value<uint32_t>(record.data(), record.size(), 44);
            descriptor.width = load_value<uint64_t>(record.data(), record.size(), 48);
            descriptor.height = load_value<uint64_t>(record.data(), record.size(), 56);
            descriptor.overview.struct_size = sizeof(descriptor.overview);
            descriptor.overview.flags = load_value<uint32_t>(record.data(), record.size(), 64);
            descriptor.overview.valid_value_count =
                load_value<uint64_t>(record.data(), record.size(), 72);
            descriptor.overview.nodata_value_count =
                load_value<uint64_t>(record.data(), record.size(), 80);
            descriptor.overview.minimum_value =
                load_value<double>(record.data(), record.size(), 88);
            descriptor.overview.maximum_value =
                load_value<double>(record.data(), record.size(), 96);
            descriptor.overview.mean_value =
                load_value<double>(record.data(), record.size(), 104);
            descriptor.payload_hash =
                load_value<uint64_t>(record.data(), record.size(), 112);
            if (descriptor.key.scale == 0 || descriptor.width == 0 ||
                descriptor.height == 0 ||
                descriptor.width > tile_width ||
                descriptor.height > tile_height ||
                descriptor.width > std::numeric_limits<uint64_t>::max() /
                    descriptor.height ||
                descriptor.width * descriptor.height >
                    std::numeric_limits<uint64_t>::max() / sizeof(double)) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "DGTILE record dimensions are invalid");
            }
            const uint64_t payload_bytes = descriptor.width *
                descriptor.height * sizeof(double);
            if (payload_bytes > file_bytes - cursor - kRecordHeaderSize) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "DGTILE record payload is truncated");
            }
            descriptor.payload_offset = cursor + kRecordHeaderSize;
            index[descriptor.key] = descriptor;
            cursor += kRecordHeaderSize + payload_bytes;
        }
        if (cursor != file_bytes) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE directory does not reach declared size");
        }
    }

    std::optional<PersistentTileDescriptor> find(
        const PersistentTileKey& key) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = index.find(key);
        if (found == index.end()) return std::nullopt;
        ++disk_hit_count;
        return found->second;
    }

    void load_tile(const PersistentTileDescriptor& descriptor,
                   std::vector<double>& values,
                   dt_grid_overview_result& overview) const {
        if (descriptor.width > std::numeric_limits<size_t>::max() /
                                   descriptor.height) {
            throw Exception(DT_E_LIMIT_EXCEEDED,
                            "DGTILE tile is too large for this process");
        }
        values.resize(static_cast<size_t>(descriptor.width * descriptor.height));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw Exception(DT_E_IO, "cannot reopen DGTILE file");
        stream.seekg(static_cast<std::streamoff>(descriptor.payload_offset));
        if (!stream) throw Exception(DT_E_CORRUPTED_DATA,
                                     "DGTILE payload offset is invalid");
        const uint64_t payload_bytes = values.size() * sizeof(double);
        read_exact(stream, values.data(), payload_bytes,
                   "cannot read DGTILE tile payload");
        if (hash_bytes(values.data(), static_cast<size_t>(payload_bytes)) !=
            descriptor.payload_hash) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DGTILE tile payload checksum mismatch");
        }
        overview = descriptor.overview;
    }

    bool discard_corrupt_payload(const PersistentTileKey& key) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!reset_corrupted || read_only) return false;
        index.erase(key);
        return true;
    }

    bool append_tile(const PersistentTileKey& key, uint64_t width,
                     uint64_t height, const std::vector<double>& values,
                     const dt_grid_overview_result& overview) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index.find(key) != index.end()) return true;
        if (read_only || write_disabled) {
            ++skipped_write_count;
            return false;
        }
        if (width == 0 || height == 0 || width > tile_width ||
            height > tile_height ||
            width > std::numeric_limits<uint64_t>::max() / height ||
            width * height != values.size()) {
            ++skipped_write_count;
            return false;
        }
        const uint64_t payload_bytes = values.size() * sizeof(double);
        if (file_bytes > maximum_file_bytes ||
            kRecordHeaderSize + payload_bytes >
                maximum_file_bytes - file_bytes) {
            ++skipped_write_count;
            return false;
        }

        std::array<unsigned char, kRecordHeaderSize> record{};
        std::memcpy(record.data(), kRecordMagic, sizeof(kRecordMagic));
        store(record.data(), record.size(), 8,
              static_cast<uint32_t>(kRecordHeaderSize));
        store(record.data(), record.size(), 12, uint32_t{0});
        store(record.data(), record.size(), 16, key.scale);
        store(record.data(), record.size(), 24, key.x);
        store(record.data(), record.size(), 32, key.y);
        store(record.data(), record.size(), 40, key.method);
        store(record.data(), record.size(), 44, key.flags);
        store(record.data(), record.size(), 48, width);
        store(record.data(), record.size(), 56, height);
        store(record.data(), record.size(), 64, overview.flags);
        store(record.data(), record.size(), 68, uint32_t{0});
        store(record.data(), record.size(), 72, overview.valid_value_count);
        store(record.data(), record.size(), 80, overview.nodata_value_count);
        store(record.data(), record.size(), 88, overview.minimum_value);
        store(record.data(), record.size(), 96, overview.maximum_value);
        store(record.data(), record.size(), 104, overview.mean_value);
        const uint64_t payload_hash = hash_bytes(
            values.data(), static_cast<size_t>(payload_bytes));
        store(record.data(), record.size(), 112, payload_hash);
        store(record.data(), record.size(), kRecordChecksumOffset, uint64_t{0});
        store(record.data(), record.size(), kRecordChecksumOffset,
              hash_bytes(record.data(), record.size()));

        const uint64_t old_file_bytes = file_bytes;
        const uint64_t new_file_bytes = file_bytes + kRecordHeaderSize +
            payload_bytes;
        const uint64_t new_record_count = record_count + 1;
        const auto new_header = make_header(new_record_count, new_file_bytes);
        try {
            writer.clear();
            writer.seekp(static_cast<std::streamoff>(old_file_bytes));
            write_exact(writer, record.data(), record.size(),
                        "cannot append DGTILE record");
            write_exact(writer, values.data(), payload_bytes,
                        "cannot append DGTILE payload");
            writer.seekp(0);
            write_exact(writer, new_header.data(), new_header.size(),
                        "cannot update DGTILE header");
            writer.flush();
            if (!writer) throw Exception(DT_E_IO,
                                         "cannot flush DGTILE record");
        } catch (...) {
            write_disabled = true;
            writer.close();
            ++skipped_write_count;
            return false;
        }

        PersistentTileDescriptor descriptor{};
        descriptor.key = key;
        descriptor.payload_offset = old_file_bytes + kRecordHeaderSize;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.payload_hash = payload_hash;
        descriptor.overview = overview;
        index.emplace(key, descriptor);
        header = new_header;
        file_bytes = new_file_bytes;
        record_count = new_record_count;
        ++written_count;
        return true;
    }

    dt_grid_view_disk_cache_statistics statistics() const {
        std::lock_guard<std::mutex> lock(mutex);
        dt_grid_view_disk_cache_statistics result{};
        result.struct_size = sizeof(result);
        result.flags = DT_GRID_VIEW_DISK_CACHE_ACTIVE;
        if (read_only)
            result.flags |= DT_GRID_VIEW_DISK_CACHE_READ_ONLY_ACTIVE;
        if (write_disabled)
            result.flags |= DT_GRID_VIEW_DISK_CACHE_WRITE_DISABLED;
        result.capacity_bytes = maximum_file_bytes;
        result.file_bytes = file_bytes;
        result.indexed_tile_count = index.size();
        result.disk_hit_tile_count = disk_hit_count;
        result.written_tile_count = written_count;
        result.skipped_write_count = skipped_write_count;
        result.source_fingerprint = source_fingerprint;
        return result;
    }
};

GridTilePackage::GridTilePackage(
    const Grid& source, uint32_t tile_width, uint32_t tile_height,
    const dt_grid_view_disk_cache_options& options)
    : impl_(std::make_unique<Impl>(source, tile_width, tile_height, options)) {}

GridTilePackage::~GridTilePackage() = default;

std::optional<PersistentTileDescriptor> GridTilePackage::find(
    const PersistentTileKey& key) {
    return impl_->find(key);
}

bool GridTilePackage::load(const PersistentTileDescriptor& descriptor,
                           std::vector<double>& values,
                           dt_grid_overview_result& overview) const {
    try {
        impl_->load_tile(descriptor, values, overview);
        return true;
    } catch (const Exception& exception) {
        if (exception.status() == DT_E_CORRUPTED_DATA &&
            impl_->discard_corrupt_payload(descriptor.key)) {
            values.clear();
            overview = {};
            return false;
        }
        throw;
    }
}

bool GridTilePackage::append(const PersistentTileKey& key, uint64_t width,
                             uint64_t height,
                             const std::vector<double>& values,
                             const dt_grid_overview_result& overview) noexcept {
    try {
        return impl_->append_tile(key, width, height, values, overview);
    } catch (...) {
        return false;
    }
}

dt_grid_view_disk_cache_statistics GridTilePackage::statistics() const {
    return impl_->statistics();
}

} // namespace dt
