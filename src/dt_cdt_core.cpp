#include "dt_cdt_core.hpp"

#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Constrained_triangulation_face_base_2.h>
#include <CGAL/Intersections_2/Iso_rectangle_2_Triangle_2.h>
#include <CGAL/Triangulation_data_structure_2.h>
#include <CGAL/Triangulation_face_base_with_info_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
#include <CGAL/number_utils.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace dt {

namespace {

constexpr uint64_t kMaxLoadPoints = 500000000ULL;
constexpr uint64_t kMaxLoadConstraints = 10000000ULL;
constexpr uint64_t kMaxLoadConstraintPoints = 500000000ULL;

struct CdtFaceInfo {
    int nesting_level = -1;
};

using CdtVertexBase =
    CGAL::Triangulation_vertex_base_with_info_2<VertexInfo, Kernel>;
using CdtConstrainedFaceBase = CGAL::Constrained_triangulation_face_base_2<Kernel>;
using CdtFaceBase = CGAL::Triangulation_face_base_with_info_2<
    CdtFaceInfo, Kernel, CdtConstrainedFaceBase>;
using CdtTds =
    CGAL::Triangulation_data_structure_2<CdtVertexBase, CdtFaceBase>;
using Cdt = CGAL::Constrained_Delaunay_triangulation_2<
    Kernel, CdtTds, CGAL::No_constraint_intersection_requiring_constructions_tag>;
using CdtFaceHandle = Cdt::Face_handle;
using CdtVertexHandle = Cdt::Vertex_handle;
using CdtEdge = Cdt::Edge;
using BarrierKey = std::pair<dt_vertex_id, dt_vertex_id>;

bool finite_value(double value) {
    return std::isfinite(value) != 0;
}

void validate_point(const dt_point3& point) {
    if (!finite_value(point.x) || !finite_value(point.y) ||
        !finite_value(point.z)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "CDT point coordinates must be finite");
    }
}

void validate_bounds(const dt_bounds2& bounds) {
    if (!finite_value(bounds.xmin) || !finite_value(bounds.ymin) ||
        !finite_value(bounds.xmax) || !finite_value(bounds.ymax) ||
        bounds.xmin > bounds.xmax || bounds.ymin > bounds.ymax) {
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid CDT query bounds");
    }
}

bool same_xy(const dt_point3& a, const dt_point3& b) {
    return a.x == b.x && a.y == b.y;
}

bool compatible_z(double a, double b) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= 1e-12 * scale;
}

BarrierKey barrier_key(CdtVertexHandle a, CdtVertexHandle b) {
    const auto x = a->info().id;
    const auto y = b->info().id;
    return x < y ? BarrierKey{x, y} : BarrierKey{y, x};
}

std::pair<CdtVertexHandle, CdtVertexHandle> edge_vertices(const CdtEdge& edge) {
    return {edge.first->vertex(Cdt::ccw(edge.second)),
            edge.first->vertex(Cdt::cw(edge.second))};
}

dt_vertex3 to_vertex(CdtVertexHandle vertex) {
    return {{CGAL::to_double(vertex->point().x()),
             CGAL::to_double(vertex->point().y()), vertex->info().z},
            vertex->info().id};
}

dt_triangle3 to_triangle(CdtFaceHandle face) {
    return {{to_vertex(face->vertex(0)), to_vertex(face->vertex(1)),
             to_vertex(face->vertex(2))}};
}

template <class StateT>
bool domain_face(const StateT& state, CdtFaceHandle face) {
    if (state.triangulation.is_infinite(face)) return false;
    return !state.has_outer_boundary ||
           (face->info().nesting_level >= 0 &&
            (face->info().nesting_level % 2) == 1);
}

template <class StateT>
void mark_component(StateT& state, CdtFaceHandle start, int nesting_level,
                    std::list<CdtEdge>& borders) {
    if (start->info().nesting_level != -1) return;
    std::list<CdtFaceHandle> queue;
    queue.push_back(start);
    while (!queue.empty()) {
        const auto face = queue.front();
        queue.pop_front();
        if (face->info().nesting_level != -1) continue;
        face->info().nesting_level = nesting_level;
        for (int i = 0; i < 3; ++i) {
            const auto neighbor = face->neighbor(i);
            if (neighbor->info().nesting_level != -1) continue;
            const CdtEdge edge(face, i);
            const auto vertices = edge_vertices(edge);
            if (state.domain_barrier_edges.count(
                    barrier_key(vertices.first, vertices.second)) != 0) {
                borders.push_back(edge);
            } else {
                queue.push_back(neighbor);
            }
        }
    }
}

template <class StateT>
void mark_domains(StateT& state) {
    for (auto face = state.triangulation.all_faces_begin();
         face != state.triangulation.all_faces_end(); ++face) {
        face->info().nesting_level = -1;
    }
    std::list<CdtEdge> borders;
    mark_component(state, state.triangulation.infinite_face(), 0, borders);
    while (!borders.empty()) {
        const CdtEdge edge = borders.front();
        borders.pop_front();
        const auto neighbor = edge.first->neighbor(edge.second);
        if (neighbor->info().nesting_level == -1) {
            mark_component(state, neighbor,
                           edge.first->info().nesting_level + 1, borders);
        }
    }
}

void require_file_name(const char* file_name) {
    if (!file_name || *file_name == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "CDT file name is empty");
    }
}

template <class T>
void read_value(std::istream& stream, T& value, const char* message) {
    if (!(stream >> value)) throw Exception(DT_E_CORRUPTED_DATA, message);
}

} // namespace

struct CdtContext::State {
    Cdt triangulation;
    std::vector<dt_point3> base_points;
    std::vector<CdtConstraint> constraints;
    std::set<BarrierKey> domain_barrier_edges;
    dt_constraint_id next_constraint_id = 1;
    uint64_t generation = 0;
    uint64_t constrained_edge_count = 0;
    uint64_t domain_triangle_count = 0;
    bool has_outer_boundary = false;
    dt_bounds2 bounds{};
    std::string crs_wkt;
};

CdtContext::CdtContext() : state_(std::make_unique<State>()) {}
CdtContext::~CdtContext() = default;

std::unique_ptr<CdtContext::State> CdtContext::make_state(
    const std::vector<dt_point3>& base_points,
    const std::vector<CdtConstraint>& constraints,
    dt_constraint_id next_constraint_id, uint64_t generation,
    const std::string& crs_wkt) {
    auto next = std::make_unique<State>();
    next->base_points = base_points;
    next->constraints = constraints;
    next->next_constraint_id = next_constraint_id;
    next->generation = generation;
    next->crs_wkt = crs_wkt;

    std::map<std::pair<double, double>, VertexInfo> vertices;
    dt_vertex_id next_vertex_id = 1;
    auto register_point = [&](const dt_point3& point, bool base_point) {
        validate_point(point);
        const auto key = std::make_pair(point.x, point.y);
        const auto found = vertices.find(key);
        if (found != vertices.end()) {
            if (base_point || !compatible_z(found->second.z, point.z)) {
                throw Exception(base_point ? DT_E_DUPLICATE_XY
                                           : DT_E_INVALID_ARGUMENT,
                                base_point
                                    ? "two CDT base points have identical XY"
                                    : "shared constraint point has inconsistent Z");
            }
            return;
        }
        vertices.emplace(key, VertexInfo{next_vertex_id++, point.z});
    };

    for (const auto& point : base_points) register_point(point, true);
    bool has_hole = false;
    std::set<dt_constraint_id> constraint_ids;
    for (const auto& constraint : constraints) {
        if (constraint.id == 0 || !constraint_ids.insert(constraint.id).second) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "invalid or duplicate CDT constraint id");
        }
        if (constraint.kind != DT_CONSTRAINT_BREAKLINE &&
            constraint.kind != DT_CONSTRAINT_OUTER_BOUNDARY &&
            constraint.kind != DT_CONSTRAINT_HOLE_BOUNDARY) {
            throw Exception(DT_E_INVALID_ARGUMENT, "unknown constraint kind");
        }
        const bool closed = (constraint.flags & DT_CONSTRAINT_CLOSED) != 0;
        const uint64_t minimum = closed ? 3 : 2;
        if (constraint.points.size() < minimum) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "constraint has too few distinct points");
        }
        if (constraint.kind != DT_CONSTRAINT_BREAKLINE && !closed) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "boundary and hole constraints must be closed");
        }
        next->has_outer_boundary |=
            constraint.kind == DT_CONSTRAINT_OUTER_BOUNDARY;
        has_hole |= constraint.kind == DT_CONSTRAINT_HOLE_BOUNDARY;
        for (size_t i = 0; i < constraint.points.size(); ++i) {
            if (i > 0 && same_xy(constraint.points[i - 1],
                                 constraint.points[i])) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "constraint contains a zero-length segment");
            }
            register_point(constraint.points[i], false);
        }
        if (closed && same_xy(constraint.points.front(),
                              constraint.points.back())) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "closed constraint must not repeat its first point");
        }
    }
    if (has_hole && !next->has_outer_boundary) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "a hole boundary requires an outer boundary");
    }

    for (const auto& item : vertices) {
        const auto handle = next->triangulation.insert(
            Kernel::Point_2(item.first.first, item.first.second));
        handle->info() = item.second;
    }

    try {
        for (const auto& constraint : constraints) {
            const size_t count = constraint.points.size();
            const size_t segment_count =
                count - 1 + ((constraint.flags & DT_CONSTRAINT_CLOSED) ? 1 : 0);
            for (size_t i = 0; i < segment_count; ++i) {
                const auto& a = constraint.points[i % count];
                const auto& b = constraint.points[(i + 1) % count];
                const auto va = next->triangulation.insert(Kernel::Point_2(a.x, a.y));
                const auto vb = next->triangulation.insert(Kernel::Point_2(b.x, b.y));
                next->triangulation.insert_constraint(va, vb);
            }
        }
    } catch (const Cdt::Intersection_of_constraints_exception&) {
        throw Exception(DT_E_UNSUPPORTED,
                        "intersecting constraint segments are not supported; "
                        "split them at a shared input vertex");
    }

    std::vector<Kernel::Segment_2> domain_segments;
    for (const auto& constraint : constraints) {
        if (constraint.kind == DT_CONSTRAINT_BREAKLINE) continue;
        const size_t count = constraint.points.size();
        for (size_t i = 0; i < count; ++i) {
            const auto& a = constraint.points[i];
            const auto& b = constraint.points[(i + 1) % count];
            domain_segments.emplace_back(Kernel::Point_2(a.x, a.y),
                                         Kernel::Point_2(b.x, b.y));
        }
    }

    for (auto edge = next->triangulation.constrained_edges_begin();
         edge != next->triangulation.constrained_edges_end(); ++edge) {
        ++next->constrained_edge_count;
        const auto vertices_on_edge = edge_vertices(*edge);
        const auto& a = vertices_on_edge.first->point();
        const auto& b = vertices_on_edge.second->point();
        for (const auto& segment : domain_segments) {
            if (segment.has_on(a) && segment.has_on(b)) {
                next->domain_barrier_edges.insert(
                    barrier_key(vertices_on_edge.first, vertices_on_edge.second));
                break;
            }
        }
    }
    mark_domains(*next);
    for (auto face = next->triangulation.finite_faces_begin();
         face != next->triangulation.finite_faces_end(); ++face) {
        if (domain_face(*next, face)) ++next->domain_triangle_count;
    }

    if (!vertices.empty()) {
        next->bounds.xmin = next->bounds.xmax = vertices.begin()->first.first;
        next->bounds.ymin = next->bounds.ymax = vertices.begin()->first.second;
        for (const auto& item : vertices) {
            next->bounds.xmin = std::min(next->bounds.xmin, item.first.first);
            next->bounds.ymin = std::min(next->bounds.ymin, item.first.second);
            next->bounds.xmax = std::max(next->bounds.xmax, item.first.first);
            next->bounds.ymax = std::max(next->bounds.ymax, item.first.second);
        }
    }
    return next;
}

void CdtContext::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    state_ = make_state({}, {}, 1, state_->generation + 1, state_->crs_wkt);
}

void CdtContext::build(const dt_point3* points, uint64_t count) {
    if (count > 0 && !points) {
        throw Exception(DT_E_INVALID_ARGUMENT, "CDT points is null");
    }
    if (count > kMaxLoadPoints) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "too many CDT base points");
    }
    std::vector<dt_point3> copied;
    if (count > 0) copied.assign(points, points + count);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto next = make_state(copied, {}, 1, state_->generation + 1,
                           state_->crs_wkt);
    state_.swap(next);
}

dt_constraint_id CdtContext::add_constraint(int32_t kind, uint32_t flags,
                                             const dt_point3* points,
                                             uint64_t count) {
    if (!points || count == 0) {
        throw Exception(DT_E_INVALID_ARGUMENT, "constraint points is empty");
    }
    if (kind != DT_CONSTRAINT_BREAKLINE &&
        kind != DT_CONSTRAINT_OUTER_BOUNDARY &&
        kind != DT_CONSTRAINT_HOLE_BOUNDARY) {
        throw Exception(DT_E_INVALID_ARGUMENT, "unknown constraint kind");
    }
    CdtConstraint constraint;
    constraint.kind = kind;
    constraint.flags = flags & DT_CONSTRAINT_CLOSED;
    if (kind != DT_CONSTRAINT_BREAKLINE) {
        constraint.flags |= DT_CONSTRAINT_CLOSED;
    }
    constraint.points.assign(points, points + count);
    if (constraint.points.size() > 1 &&
        same_xy(constraint.points.front(), constraint.points.back())) {
        if (!compatible_z(constraint.points.front().z,
                          constraint.points.back().z)) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "repeated closing point has inconsistent Z");
        }
        constraint.points.pop_back();
        constraint.flags |= DT_CONSTRAINT_CLOSED;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    constraint.id = state_->next_constraint_id;
    if (constraint.id == 0 || constraint.id ==
                                  std::numeric_limits<dt_constraint_id>::max()) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "constraint id space exhausted");
    }
    auto constraints = state_->constraints;
    constraints.push_back(constraint);
    auto next = make_state(state_->base_points, constraints, constraint.id + 1,
                           state_->generation + 1, state_->crs_wkt);
    state_.swap(next);
    return constraint.id;
}

void CdtContext::remove_constraint(dt_constraint_id id) {
    if (id == 0) throw Exception(DT_E_INVALID_ARGUMENT, "constraint id is zero");
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto constraints = state_->constraints;
    const auto found = std::find_if(
        constraints.begin(), constraints.end(),
        [id](const CdtConstraint& constraint) { return constraint.id == id; });
    if (found == constraints.end()) {
        throw Exception(DT_E_NOT_FOUND, "constraint id was not found");
    }
    constraints.erase(found);
    auto next = make_state(state_->base_points, constraints,
                           state_->next_constraint_id, state_->generation + 1,
                           state_->crs_wkt);
    state_.swap(next);
}

dt_cdt_statistics CdtContext::statistics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    dt_cdt_statistics result{};
    result.struct_size = sizeof(result);
    result.base_point_count = state_->base_points.size();
    result.vertex_count = state_->triangulation.number_of_vertices();
    result.finite_triangle_count = state_->triangulation.number_of_faces();
    result.domain_triangle_count = state_->domain_triangle_count;
    result.constraint_count = state_->constraints.size();
    result.constrained_edge_count = state_->constrained_edge_count;
    result.generation = state_->generation;
    result.bounds = state_->bounds;
    return result;
}

CdtConstraint CdtContext::constraint_at(uint64_t index) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (index >= state_->constraints.size()) {
        throw Exception(DT_E_NOT_FOUND, "constraint index is out of range");
    }
    return state_->constraints[static_cast<size_t>(index)];
}

CdtConstraint CdtContext::constraint_by_id(dt_constraint_id id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = std::find_if(
        state_->constraints.begin(), state_->constraints.end(),
        [id](const CdtConstraint& constraint) { return constraint.id == id; });
    if (found == state_->constraints.end()) {
        throw Exception(DT_E_NOT_FOUND, "constraint id was not found");
    }
    return *found;
}

std::unique_ptr<CdtQueryData> CdtContext::query(
    const dt_bounds2& bounds) const {
    validate_bounds(bounds);
    auto result = std::make_unique<CdtQueryData>();
    std::shared_lock<std::shared_mutex> lock(mutex_);
    result->generation = state_->generation;
    const Kernel::Iso_rectangle_2 rectangle(bounds.xmin, bounds.ymin,
                                             bounds.xmax, bounds.ymax);
    for (auto face = state_->triangulation.finite_faces_begin();
         face != state_->triangulation.finite_faces_end(); ++face) {
        if (!domain_face(*state_, face)) continue;
        const Kernel::Triangle_2 triangle(face->vertex(0)->point(),
                                          face->vertex(1)->point(),
                                          face->vertex(2)->point());
        if (CGAL::do_intersect(rectangle, triangle)) {
            result->triangles.push_back(to_triangle(face));
        }
    }
    return result;
}

bool CdtContext::validate(bool verbose) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!state_->triangulation.is_valid(verbose)) return false;
    uint64_t domain_count = 0;
    for (auto face = state_->triangulation.finite_faces_begin();
         face != state_->triangulation.finite_faces_end(); ++face) {
        if (domain_face(*state_, face)) ++domain_count;
    }
    return domain_count == state_->domain_triangle_count;
}

void CdtContext::set_crs_wkt(std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    state_->crs_wkt = std::move(value);
}

std::string CdtContext::crs_wkt() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return state_->crs_wkt;
}

void CdtContext::save_text(const char* utf8_file_name) const {
    require_file_name(utf8_file_name);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::ofstream output(utf8_file_name, std::ios::binary | std::ios::trunc);
    if (!output) throw Exception(DT_E_IO, "cannot create CDT text file");
    output.imbue(std::locale::classic());
    output << std::setprecision(17);
    output << "DCDT 1\n";
    output << "CRS " << std::quoted(state_->crs_wkt) << "\n";
    output << "POINTS " << state_->base_points.size() << "\n";
    for (const auto& point : state_->base_points) {
        output << point.x << ' ' << point.y << ' ' << point.z << "\n";
    }
    output << "CONSTRAINTS " << state_->constraints.size() << "\n";
    for (const auto& constraint : state_->constraints) {
        output << "CONSTRAINT " << constraint.id << ' ' << constraint.kind << ' '
               << constraint.flags << ' ' << constraint.points.size() << "\n";
        for (const auto& point : constraint.points) {
            output << point.x << ' ' << point.y << ' ' << point.z << "\n";
        }
    }
    output << "END\n";
    if (!output) throw Exception(DT_E_IO, "failed while writing CDT text file");
}

dt_bounds2 CdtContext::load_text(const char* utf8_file_name) {
    require_file_name(utf8_file_name);
    std::ifstream input(utf8_file_name, std::ios::binary);
    if (!input) throw Exception(DT_E_IO, "cannot open CDT text file");
    input.imbue(std::locale::classic());

    std::string token;
    uint32_t version = 0;
    read_value(input, token, "missing DCDT header");
    read_value(input, version, "missing DCDT version");
    if (token != "DCDT" || version != 1) {
        throw Exception(DT_E_CORRUPTED_DATA, "unsupported DCDT text header");
    }
    read_value(input, token, "missing DCDT CRS record");
    if (token != "CRS") {
        throw Exception(DT_E_CORRUPTED_DATA, "expected DCDT CRS record");
    }
    std::string crs;
    if (!(input >> std::quoted(crs))) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid quoted DCDT CRS");
    }

    read_value(input, token, "missing DCDT points record");
    if (token != "POINTS") {
        throw Exception(DT_E_CORRUPTED_DATA, "expected DCDT POINTS record");
    }
    uint64_t point_count = 0;
    read_value(input, point_count, "invalid DCDT point count");
    if (point_count > kMaxLoadPoints) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "DCDT point count is too large");
    }
    std::vector<dt_point3> points(static_cast<size_t>(point_count));
    for (auto& point : points) {
        read_value(input, point.x, "truncated DCDT point X");
        read_value(input, point.y, "truncated DCDT point Y");
        read_value(input, point.z, "truncated DCDT point Z");
    }

    read_value(input, token, "missing DCDT constraints record");
    if (token != "CONSTRAINTS") {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "expected DCDT CONSTRAINTS record");
    }
    uint64_t constraint_count = 0;
    read_value(input, constraint_count, "invalid DCDT constraint count");
    if (constraint_count > kMaxLoadConstraints) {
        throw Exception(DT_E_LIMIT_EXCEEDED,
                        "DCDT constraint count is too large");
    }
    std::vector<CdtConstraint> constraints;
    constraints.reserve(static_cast<size_t>(constraint_count));
    uint64_t total_constraint_points = 0;
    dt_constraint_id next_constraint_id = 1;
    for (uint64_t i = 0; i < constraint_count; ++i) {
        read_value(input, token, "truncated DCDT constraint record");
        if (token != "CONSTRAINT") {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "expected DCDT CONSTRAINT record");
        }
        CdtConstraint constraint;
        uint64_t count = 0;
        read_value(input, constraint.id, "invalid DCDT constraint id");
        if (constraint.id == std::numeric_limits<dt_constraint_id>::max()) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DCDT constraint id leaves no successor id");
        }
        read_value(input, constraint.kind, "invalid DCDT constraint kind");
        read_value(input, constraint.flags, "invalid DCDT constraint flags");
        read_value(input, count, "invalid DCDT constraint point count");
        if (count > kMaxLoadConstraintPoints - total_constraint_points) {
            throw Exception(DT_E_LIMIT_EXCEEDED,
                            "DCDT constraint point count is too large");
        }
        total_constraint_points += count;
        constraint.points.resize(static_cast<size_t>(count));
        for (auto& point : constraint.points) {
            read_value(input, point.x, "truncated constraint point X");
            read_value(input, point.y, "truncated constraint point Y");
            read_value(input, point.z, "truncated constraint point Z");
        }
        next_constraint_id = std::max(next_constraint_id, constraint.id + 1);
        constraints.push_back(std::move(constraint));
    }
    read_value(input, token, "missing DCDT END record");
    if (token != "END") {
        throw Exception(DT_E_CORRUPTED_DATA, "expected DCDT END record");
    }
    input >> std::ws;
    if (!input.eof()) {
        throw Exception(DT_E_CORRUPTED_DATA, "unexpected data after DCDT END");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto next = make_state(points, constraints, next_constraint_id,
                           state_->generation + 1, crs);
    const auto bounds = next->bounds;
    state_.swap(next);
    return bounds;
}

} // namespace dt
