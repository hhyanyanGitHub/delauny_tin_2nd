#ifndef DT_CORE_HPP
#define DT_CORE_HPP

#include "dt_api.h"

#include <CGAL/Delaunay_triangulation_2.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Triangulation_data_structure_2.h>
#include <CGAL/Triangulation_face_base_with_info_2.h>
#include <CGAL/Triangulation_hierarchy_2.h>
#include <CGAL/Triangulation_hierarchy_vertex_base_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dt {

class Exception final : public std::runtime_error {
public:
    Exception(dt_status status, const std::string& message)
        : std::runtime_error(message), status_(status) {}
    dt_status status() const noexcept { return status_; }

private:
    dt_status status_;
};

struct VertexInfo {
    dt_vertex_id id = 0;
    double z = 0.0;
};

struct FaceInfo {
    uint64_t reserved = 0;
};

using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using VertexInfoBase = CGAL::Triangulation_vertex_base_with_info_2<VertexInfo, Kernel>;
using VertexBase = CGAL::Triangulation_hierarchy_vertex_base_2<VertexInfoBase>;
using FaceBase = CGAL::Triangulation_face_base_with_info_2<FaceInfo, Kernel>;
using Tds = CGAL::Triangulation_data_structure_2<VertexBase, FaceBase>;
using DelaunayBase = CGAL::Delaunay_triangulation_2<Kernel, Tds>;
using Hierarchy = CGAL::Triangulation_hierarchy_2<DelaunayBase>;

class Triangulation final : public Hierarchy {
public:
    using Hierarchy::Hierarchy;
};

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;
using IndexPoint = bg::model::point<double, 2, bg::cs::cartesian>;
using IndexBox = bg::model::box<IndexPoint>;
using FaceHandle = Triangulation::Face_handle;
using VertexHandle = Triangulation::Vertex_handle;
using IndexValue = std::pair<IndexBox, FaceHandle>;
using FaceIndex = bgi::rtree<IndexValue, bgi::rstar<16>>;

struct EditData {
    std::vector<dt_triangle3> removed_triangles;
    std::vector<dt_triangle3> added_triangles;
    std::vector<dt_segment3> boundary_edges;
    std::vector<dt_segment3> removed_edges;
    std::vector<dt_segment3> added_edges;
    dt_vertex_id affected_vertex_id = 0;
    uint64_t generation = 0;
};

struct QueryData {
    std::vector<dt_triangle3> triangles;
    uint64_t generation = 0;
};

class Context final {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void clear();
    void build(const dt_point3* points, uint64_t count, dt_vertex_id* output_ids);
    std::unique_ptr<EditData> insert(const dt_point3& point, dt_vertex_id* output_id);
    std::unique_ptr<EditData> delete_nearest(const dt_point3& query,
                                            dt_vertex_id* deleted_id);
    std::unique_ptr<EditData> delete_vertex(dt_vertex_id id);
    void update_z(dt_vertex_id id, double z);

    dt_vertex3 nearest(const dt_point3& query) const;
    dt_location_result locate(const dt_point3& query) const;
    dt_surface_analysis analyze_surface_xy(const dt_point3& query) const;
    std::vector<dt_point3> points() const;
    std::unique_ptr<QueryData> query(const dt_bounds2& bounds) const;
    void visit_triangles(
        const std::function<void(const dt_triangle3&)>& visitor) const;
    dt_statistics statistics() const;
    bool validate(bool verbose) const;
    void set_crs_wkt(std::string crs_wkt);
    std::string crs_wkt() const;

    void save(const char* utf8_file_name) const;
    dt_bounds2 load(const char* utf8_file_name);
    dt_bounds2 import_points_text(const char* utf8_file_name);
    void save_mesh_text(const char* utf8_file_name) const;
    dt_bounds2 load_mesh_text(const char* utf8_file_name);

private:
    struct State;
    std::unique_ptr<State> state_;
    mutable std::shared_mutex mutex_;

    static void validate_point(const dt_point3& point);
    static void validate_bounds(const dt_bounds2& bounds);
    static dt_vertex3 to_vertex(VertexHandle vertex);
    static dt_segment3 to_segment(VertexHandle a, VertexHandle b);
    static dt_triangle3 to_triangle(FaceHandle face);
    static IndexBox face_box(FaceHandle face);

    static std::unique_ptr<State> make_state(const dt_point3* points,
                                             const dt_vertex_id* ids,
                                             uint64_t count,
                                             dt_vertex_id* output_ids);
    static void rebuild_index(State& state);
    static void add_face_to_index(State& state, FaceHandle face);
    static void remove_face_from_index(State& state, FaceHandle face);
    static bool finite_face(const State& state, FaceHandle face);

    std::unique_ptr<EditData> delete_vertex_locked(VertexHandle vertex);
};

} // namespace dt

#endif
