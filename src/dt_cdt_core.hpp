#ifndef DT_CDT_CORE_HPP
#define DT_CDT_CORE_HPP

#include "dt_cdt_api.h"
#include "dt_core.hpp"

#include <memory>
#include <shared_mutex>
#include <string>
#include <functional>
#include <vector>

namespace dt {

struct CdtConstraint {
    dt_constraint_id id = 0;
    int32_t kind = DT_CONSTRAINT_BREAKLINE;
    uint32_t flags = 0;
    std::vector<dt_point3> points;
};

struct CdtQueryData {
    std::vector<dt_triangle3> triangles;
    uint64_t generation = 0;
};

class CdtContext final {
public:
    CdtContext();
    ~CdtContext();
    CdtContext(const CdtContext&) = delete;
    CdtContext& operator=(const CdtContext&) = delete;

    void clear();
    void build(const dt_point3* points, uint64_t count);
    void build_from_tin(std::vector<dt_point3> points, std::string crs_wkt);
    dt_constraint_id add_constraint(int32_t kind, uint32_t flags,
                                    const dt_point3* points, uint64_t count);
    void remove_constraint(dt_constraint_id id);

    dt_cdt_statistics statistics() const;
    CdtConstraint constraint_at(uint64_t index) const;
    CdtConstraint constraint_by_id(dt_constraint_id id) const;
    std::unique_ptr<CdtQueryData> query(const dt_bounds2& bounds) const;
    double sample_height_xy(const dt_point3& query) const;
    void visit_domain_triangles(
        const std::function<void(const dt_triangle3&)>& visitor) const;
    bool validate(bool verbose) const;

    void set_crs_wkt(std::string value);
    std::string crs_wkt() const;
    void save_text(const char* utf8_file_name) const;
    dt_bounds2 load_text(const char* utf8_file_name);

private:
    struct State;
    std::unique_ptr<State> state_;
    mutable std::shared_mutex mutex_;

    static std::unique_ptr<State> make_state(
        const std::vector<dt_point3>& base_points,
        const std::vector<CdtConstraint>& constraints,
        dt_constraint_id next_constraint_id, uint64_t generation,
        const std::string& crs_wkt);
};

} // namespace dt

#endif
