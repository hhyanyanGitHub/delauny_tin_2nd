#ifndef DT_GRID_STORAGE_HPP
#define DT_GRID_STORAGE_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace dt {

class GridStorage final {
public:
    using iterator = double*;
    using const_iterator = const double*;

    GridStorage();
    ~GridStorage();
    GridStorage(const GridStorage&) = delete;
    GridStorage& operator=(const GridStorage&) = delete;

    void assign(size_t count, double value);
    void map_copy_on_write(const std::filesystem::path& file,
                           size_t byte_offset, size_t count);
    bool maps_file(const std::filesystem::path& file) const;
    void materialize();

    bool is_mapped() const noexcept;
    size_t size() const noexcept;
    double* data() noexcept;
    const double* data() const noexcept;
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size(); }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size(); }
    double& operator[](size_t index) noexcept { return data()[index]; }
    const double& operator[](size_t index) const noexcept {
        return data()[index];
    }

private:
    struct Mapping;
    std::vector<double> owned_;
    std::unique_ptr<Mapping> mapping_;
};

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination);

} // namespace dt

#endif
