#include "dt_core.hpp"

#include <CGAL/Intersections_2/Iso_rectangle_2_Triangle_2.h>
#include <CGAL/number_utils.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace dt {

namespace {

constexpr char kFileMagic[8] = {'D', 'T', 'I', 'N', '2', 'D', '\0', '\0'};
constexpr uint32_t kFileVersion = 1;
constexpr uint64_t kMaxLoadVertices = 500000000ULL;

/* The installed MinGW/CGAL combination cannot use non-trivial TLS safely.
 * Serializing CGAL entry points also gives identical behavior on all builds. */
std::recursive_mutex g_cgal_mutex;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t vertex_count;
    uint64_t reserved[4];
};

struct FileVertex {
    uint64_t id;
    double x;
    double y;
    double z;
};
#pragma pack(pop)

struct EdgeKey {
    dt_vertex_id a;
    dt_vertex_id b;

    EdgeKey(dt_vertex_id x, dt_vertex_id y)
        : a(std::min(x, y)), b(std::max(x, y)) {}

    bool operator<(const EdgeKey& other) const noexcept {
        return a < other.a || (a == other.a && b < other.b);
    }
};

struct FaceKey {
    std::array<dt_vertex_id, 3> ids{};

    bool operator<(const FaceKey& other) const noexcept {
        return ids < other.ids;
    }
};

FaceKey make_face_key(FaceHandle face) {
    FaceKey key{{face->vertex(0)->info().id,
                 face->vertex(1)->info().id,
                 face->vertex(2)->info().id}};
    std::sort(key.ids.begin(), key.ids.end());
    return key;
}

bool finite(double value) {
    return std::isfinite(value) != 0;
}

template <class T>
void checked_write(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!stream) {
        throw Exception(DT_E_IO, "failed while writing triangulation file");
    }
}

template <class T>
void checked_read(std::ifstream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!stream) {
        throw Exception(DT_E_CORRUPTED_DATA, "truncated triangulation file");
    }
}

void require_file_name(const char* file_name) {
    if (!file_name || *file_name == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "file name is empty");
    }
}

bool point_text_delimiter(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0 ||
           value == ',' || value == ';';
}

bool parse_point_text_line(const std::string& line, uint64_t line_number,
                           dt_point3& output) {
    const char* cursor = line.c_str();
    const char* const end = cursor + line.size();
    auto skip_delimiters = [&] {
        while (cursor < end && point_text_delimiter(*cursor)) ++cursor;
    };

    skip_delimiters();
    if (cursor == end || *cursor == '#') return false;

    double values[3]{};
    for (int coordinate = 0; coordinate < 3; ++coordinate) {
        errno = 0;
        char* parsed_end = nullptr;
        values[coordinate] = std::strtod(cursor, &parsed_end);
        if (parsed_end == cursor || errno == ERANGE ||
            !std::isfinite(values[coordinate])) {
            std::ostringstream message;
            message << "invalid XYZ value at line " << line_number;
            throw Exception(DT_E_CORRUPTED_DATA, message.str());
        }
        cursor = parsed_end;
        if (cursor < end && *cursor != '#' && !point_text_delimiter(*cursor)) {
            std::ostringstream message;
            message << "invalid XYZ separator at line " << line_number;
            throw Exception(DT_E_CORRUPTED_DATA, message.str());
        }
        skip_delimiters();
        if (coordinate < 2 && (cursor == end || *cursor == '#')) {
            std::ostringstream message;
            message << "expected three coordinates at line " << line_number;
            throw Exception(DT_E_CORRUPTED_DATA, message.str());
        }
    }
    if (cursor != end && *cursor != '#') {
        std::ostringstream message;
        message << "too many fields at line " << line_number;
        throw Exception(DT_E_CORRUPTED_DATA, message.str());
    }
    output = {values[0], values[1], values[2]};
    return true;
}

} // namespace

struct Context::State {
    Triangulation triangulation;
    FaceIndex face_index;
    std::vector<VertexHandle> vertex_by_id{VertexHandle()};
    dt_vertex_id next_vertex_id = 1;
    uint64_t generation = 0;
};

Context::Context() : state_(std::make_unique<State>()) {}
Context::~Context() = default;

void Context::validate_point(const dt_point3& point) {
    if (!finite(point.x) || !finite(point.y) || !finite(point.z)) {
        throw Exception(DT_E_INVALID_ARGUMENT, "point coordinates must be finite");
    }
}

void Context::validate_bounds(const dt_bounds2& bounds) {
    if (!finite(bounds.xmin) || !finite(bounds.ymin) ||
        !finite(bounds.xmax) || !finite(bounds.ymax) ||
        bounds.xmin > bounds.xmax || bounds.ymin > bounds.ymax) {
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid XY query bounds");
    }
}

dt_vertex3 Context::to_vertex(VertexHandle vertex) {
    const auto& p = vertex->point();
    return dt_vertex3{{CGAL::to_double(p.x()), CGAL::to_double(p.y()),
                       vertex->info().z},
                      vertex->info().id};
}

dt_segment3 Context::to_segment(VertexHandle a, VertexHandle b) {
    return dt_segment3{{to_vertex(a), to_vertex(b)}};
}

dt_triangle3 Context::to_triangle(FaceHandle face) {
    return dt_triangle3{{to_vertex(face->vertex(0)),
                         to_vertex(face->vertex(1)),
                         to_vertex(face->vertex(2))}};
}

IndexBox Context::face_box(FaceHandle face) {
    const auto p0 = face->vertex(0)->point();
    const auto p1 = face->vertex(1)->point();
    const auto p2 = face->vertex(2)->point();
    const double xmin = std::min({CGAL::to_double(p0.x()), CGAL::to_double(p1.x()),
                                  CGAL::to_double(p2.x())});
    const double ymin = std::min({CGAL::to_double(p0.y()), CGAL::to_double(p1.y()),
                                  CGAL::to_double(p2.y())});
    const double xmax = std::max({CGAL::to_double(p0.x()), CGAL::to_double(p1.x()),
                                  CGAL::to_double(p2.x())});
    const double ymax = std::max({CGAL::to_double(p0.y()), CGAL::to_double(p1.y()),
                                  CGAL::to_double(p2.y())});
    return IndexBox(IndexPoint(xmin, ymin), IndexPoint(xmax, ymax));
}

bool Context::finite_face(const State& state, FaceHandle face) {
    return face != FaceHandle() && !state.triangulation.is_infinite(face);
}

void Context::add_face_to_index(State& state, FaceHandle face) {
    if (finite_face(state, face)) {
        state.face_index.insert(IndexValue(face_box(face), face));
    }
}

void Context::remove_face_from_index(State& state, FaceHandle face) {
    if (finite_face(state, face)) {
        state.face_index.remove(IndexValue(face_box(face), face));
    }
}

void Context::rebuild_index(State& state) {
    std::vector<IndexValue> values;
    values.reserve(static_cast<size_t>(state.triangulation.number_of_faces()));
    for (auto it = state.triangulation.finite_faces_begin();
         it != state.triangulation.finite_faces_end(); ++it) {
        values.emplace_back(face_box(it), it);
    }
    state.face_index = FaceIndex(values.begin(), values.end());
}

std::unique_ptr<Context::State> Context::make_state(
    const dt_point3* points, const dt_vertex_id* ids, uint64_t count,
    dt_vertex_id* output_ids) {
    auto next = std::make_unique<State>();
    if (count == 0) {
        return next;
    }

    std::vector<uint64_t> order(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        validate_point(points[i]);
        order[static_cast<size_t>(i)] = i;
    }
    std::sort(order.begin(), order.end(), [points](uint64_t a, uint64_t b) {
        if (points[a].x != points[b].x) return points[a].x < points[b].x;
        return points[a].y < points[b].y;
    });
    for (size_t i = 1; i < order.size(); ++i) {
        const auto& a = points[order[i - 1]];
        const auto& b = points[order[i]];
        if (a.x == b.x && a.y == b.y) {
            throw Exception(DT_E_DUPLICATE_XY,
                            "two input points have identical XY coordinates");
        }
    }

    std::vector<Kernel::Point_2> cgal_points;
    cgal_points.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        cgal_points.emplace_back(points[i].x, points[i].y);
    }
    next->triangulation.insert(cgal_points.begin(), cgal_points.end());

    dt_vertex_id max_id = 0;
    std::vector<dt_vertex_id> assigned(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        const dt_vertex_id id = ids ? ids[i] : (i + 1);
        if (id == 0) {
            throw Exception(DT_E_CORRUPTED_DATA, "vertex ID zero is reserved");
        }
        assigned[static_cast<size_t>(i)] = id;
        max_id = std::max(max_id, id);
    }
    if (max_id > count * 8 + 1024) {
        throw Exception(DT_E_CORRUPTED_DATA, "vertex IDs are unreasonably sparse");
    }
    next->vertex_by_id.resize(static_cast<size_t>(max_id + 1), VertexHandle());

    for (uint64_t i = 0; i < count; ++i) {
        Triangulation::Locate_type locate_type;
        int locate_index = 0;
        auto face = next->triangulation.locate(cgal_points[static_cast<size_t>(i)],
                                               locate_type, locate_index);
        if (locate_type != Triangulation::VERTEX) {
            throw Exception(DT_E_INTERNAL, "failed to locate a newly built vertex");
        }
        auto vertex = face->vertex(locate_index);
        const auto id = assigned[static_cast<size_t>(i)];
        if (next->vertex_by_id[static_cast<size_t>(id)] != VertexHandle()) {
            throw Exception(DT_E_CORRUPTED_DATA, "duplicate vertex ID in file");
        }
        vertex->info() = VertexInfo{id, points[i].z};
        next->vertex_by_id[static_cast<size_t>(id)] = vertex;
        if (output_ids) output_ids[i] = id;
    }

    next->next_vertex_id = max_id + 1;
    next->generation = 1;
    rebuild_index(*next);
    return next;
}

void Context::clear() {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const uint64_t generation = state_->generation + 1;
    state_ = std::make_unique<State>();
    state_->generation = generation;
}

void Context::build(const dt_point3* points, uint64_t count,
                    dt_vertex_id* output_ids) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    if (count != 0 && points == nullptr) {
        throw Exception(DT_E_INVALID_ARGUMENT, "points is null for a non-empty build");
    }
    auto next = make_state(points, nullptr, count, output_ids);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    next->generation = state_->generation + 1;
    state_.swap(next);
}

std::unique_ptr<EditData> Context::insert(const dt_point3& point,
                                          dt_vertex_id* output_id) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    validate_point(point);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& state = *state_;
    const Kernel::Point_2 p(point.x, point.y);

    if (state.triangulation.number_of_vertices() != 0) {
        Triangulation::Locate_type type;
        int index = 0;
        state.triangulation.locate(p, type, index);
        if (type == Triangulation::VERTEX) {
            throw Exception(DT_E_DUPLICATE_XY,
                            "a vertex already exists at the supplied XY coordinate");
        }
    }

    auto result = std::make_unique<EditData>();
    std::vector<FaceHandle> conflicts;
    std::vector<Triangulation::Edge> boundary;
    if (state.triangulation.dimension() == 2) {
        state.triangulation.get_conflicts_and_boundary(
            p, std::back_inserter(conflicts), std::back_inserter(boundary));
    }

    for (auto face : conflicts) {
        if (finite_face(state, face)) {
            result->removed_triangles.push_back(to_triangle(face));
            remove_face_from_index(state, face);
        }
    }
    for (const auto& edge : boundary) {
        auto face = edge.first;
        const int i = edge.second;
        auto a = face->vertex(Triangulation::cw(i));
        auto b = face->vertex(Triangulation::ccw(i));
        if (!state.triangulation.is_infinite(a) &&
            !state.triangulation.is_infinite(b)) {
            result->boundary_edges.push_back(to_segment(a, b));
        }
    }

    auto vertex = state.triangulation.insert(p);
    const dt_vertex_id id = state.next_vertex_id++;
    vertex->info() = VertexInfo{id, point.z};
    if (state.vertex_by_id.size() <= id) {
        state.vertex_by_id.resize(static_cast<size_t>(id + 1), VertexHandle());
    }
    state.vertex_by_id[static_cast<size_t>(id)] = vertex;

    std::set<EdgeKey> added_edge_keys;
    if (state.triangulation.dimension() == 2) {
        auto incident = state.triangulation.incident_faces(vertex);
        if (incident != 0) {
            auto begin = incident;
            do {
                FaceHandle face = incident;
                if (finite_face(state, face)) {
                    result->added_triangles.push_back(to_triangle(face));
                    add_face_to_index(state, face);
                    for (int i = 0; i < 3; ++i) {
                        auto a = face->vertex(Triangulation::cw(i));
                        auto b = face->vertex(Triangulation::ccw(i));
                        if (a != vertex && b != vertex) continue;
                        EdgeKey key(a->info().id, b->info().id);
                        if (added_edge_keys.insert(key).second) {
                            result->added_edges.push_back(to_segment(a, b));
                        }
                    }
                }
                ++incident;
            } while (incident != begin);
        }
    }

    ++state.generation;
    result->affected_vertex_id = id;
    result->generation = state.generation;
    if (output_id) *output_id = id;
    return result;
}

std::unique_ptr<EditData> Context::delete_nearest(
    const dt_point3& query_point, dt_vertex_id* deleted_id) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    validate_point(query_point);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (state_->triangulation.number_of_vertices() == 0) {
        throw Exception(DT_E_EMPTY, "cannot delete from an empty triangulation");
    }
    auto vertex = state_->triangulation.nearest_vertex(
        Kernel::Point_2(query_point.x, query_point.y));
    const auto id = vertex->info().id;
    auto result = delete_vertex_locked(vertex);
    if (deleted_id) *deleted_id = id;
    return result;
}

std::unique_ptr<EditData> Context::delete_vertex(dt_vertex_id id) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (id == 0 || id >= state_->vertex_by_id.size() ||
        state_->vertex_by_id[static_cast<size_t>(id)] == VertexHandle()) {
        throw Exception(DT_E_NOT_FOUND, "vertex ID was not found");
    }
    return delete_vertex_locked(state_->vertex_by_id[static_cast<size_t>(id)]);
}

std::unique_ptr<EditData> Context::delete_vertex_locked(VertexHandle vertex) {
    auto& state = *state_;
    auto result = std::make_unique<EditData>();
    const dt_vertex_id id = vertex->info().id;

    std::set<EdgeKey> boundary_keys;
    std::set<EdgeKey> removed_edge_keys;
    std::vector<VertexHandle> neighbors;
    if (state.triangulation.dimension() == 2) {
        auto faces = state.triangulation.incident_faces(vertex);
        if (faces != 0) {
            auto begin = faces;
            do {
                auto face = faces;
                if (finite_face(state, face)) {
                    result->removed_triangles.push_back(to_triangle(face));
                    remove_face_from_index(state, face);
                    int vertex_index = 0;
                    while (vertex_index < 3 && face->vertex(vertex_index) != vertex) {
                        ++vertex_index;
                    }
                    if (vertex_index < 3) {
                        auto a = face->vertex(Triangulation::cw(vertex_index));
                        auto b = face->vertex(Triangulation::ccw(vertex_index));
                        EdgeKey boundary_key(a->info().id, b->info().id);
                        if (boundary_keys.insert(boundary_key).second) {
                            result->boundary_edges.push_back(to_segment(a, b));
                        }
                    }
                }
                ++faces;
            } while (faces != begin);
        }
    }

    auto vertices = state.triangulation.incident_vertices(vertex);
    if (vertices != 0) {
        auto begin = vertices;
        do {
            auto neighbor = vertices;
            if (!state.triangulation.is_infinite(neighbor)) {
                neighbors.push_back(neighbor);
                EdgeKey key(id, neighbor->info().id);
                if (removed_edge_keys.insert(key).second) {
                    result->removed_edges.push_back(to_segment(vertex, neighbor));
                }
            }
            ++vertices;
        } while (vertices != begin);
    }

    std::set<FaceKey> old_local_faces;
    for (auto neighbor : neighbors) {
        auto incident = state.triangulation.incident_faces(neighbor);
        if (incident == 0) continue;
        auto begin = incident;
        do {
            FaceHandle face = incident;
            if (finite_face(state, face)) old_local_faces.insert(make_face_key(face));
            ++incident;
        } while (incident != begin);
    }

    state.triangulation.remove(vertex);
    state.vertex_by_id[static_cast<size_t>(id)] = VertexHandle();

    std::set<EdgeKey> added_keys;
    std::set<FaceKey> emitted_faces;
    for (auto neighbor : neighbors) {
        auto incident = state.triangulation.incident_faces(neighbor);
        if (incident == 0) continue;
        auto begin = incident;
        do {
            FaceHandle face = incident;
            if (finite_face(state, face)) {
                const FaceKey face_key = make_face_key(face);
                if (old_local_faces.find(face_key) == old_local_faces.end() &&
                    emitted_faces.insert(face_key).second) {
                    result->added_triangles.push_back(to_triangle(face));
                    add_face_to_index(state, face);
                    for (int i = 0; i < 3; ++i) {
                        auto a = face->vertex(Triangulation::cw(i));
                        auto b = face->vertex(Triangulation::ccw(i));
                        EdgeKey key(a->info().id, b->info().id);
                        if (boundary_keys.find(key) == boundary_keys.end() &&
                            added_keys.insert(key).second) {
                            result->added_edges.push_back(to_segment(a, b));
                        }
                    }
                }
            }
            ++incident;
        } while (incident != begin);
    }

    ++state.generation;
    result->affected_vertex_id = id;
    result->generation = state.generation;
    return result;
}

void Context::update_z(dt_vertex_id id, double z) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    if (!finite(z)) {
        throw Exception(DT_E_INVALID_ARGUMENT, "elevation must be finite");
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (id == 0 || id >= state_->vertex_by_id.size() ||
        state_->vertex_by_id[static_cast<size_t>(id)] == VertexHandle()) {
        throw Exception(DT_E_NOT_FOUND, "vertex ID was not found");
    }
    state_->vertex_by_id[static_cast<size_t>(id)]->info().z = z;
    ++state_->generation;
}

dt_vertex3 Context::nearest(const dt_point3& query_point) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    validate_point(query_point);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (state_->triangulation.number_of_vertices() == 0) {
        throw Exception(DT_E_EMPTY, "nearest-vertex query on an empty triangulation");
    }
    auto vertex = state_->triangulation.nearest_vertex(
        Kernel::Point_2(query_point.x, query_point.y));
    return to_vertex(vertex);
}

dt_location_result Context::locate(const dt_point3& query_point) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    validate_point(query_point);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    dt_location_result result{};
    result.struct_size = sizeof(result);
    if (state_->triangulation.number_of_vertices() == 0) {
        result.type = DT_LOCATION_EMPTY;
        return result;
    }

    Triangulation::Locate_type type;
    int index = 0;
    auto face = state_->triangulation.locate(
        Kernel::Point_2(query_point.x, query_point.y), type, index);
    switch (type) {
    case Triangulation::FACE:
        if (finite_face(*state_, face)) {
            result.type = DT_LOCATION_FACE;
            result.triangle = to_triangle(face);
        } else {
            result.type = DT_LOCATION_OUTSIDE_HULL;
        }
        break;
    case Triangulation::EDGE: {
        result.type = DT_LOCATION_EDGE;
        auto a = face->vertex(Triangulation::cw(index));
        auto b = face->vertex(Triangulation::ccw(index));
        if (!state_->triangulation.is_infinite(a) &&
            !state_->triangulation.is_infinite(b)) {
            result.edge = to_segment(a, b);
        }
        if (finite_face(*state_, face)) result.triangle = to_triangle(face);
        break;
    }
    case Triangulation::VERTEX:
        result.type = DT_LOCATION_VERTEX;
        result.vertex = to_vertex(face->vertex(index));
        break;
    case Triangulation::OUTSIDE_CONVEX_HULL:
        result.type = DT_LOCATION_OUTSIDE_HULL;
        break;
    case Triangulation::OUTSIDE_AFFINE_HULL:
        result.type = DT_LOCATION_OUTSIDE_AFFINE_HULL;
        break;
    default:
        result.type = DT_LOCATION_EMPTY;
        break;
    }
    return result;
}

std::unique_ptr<QueryData> Context::query(const dt_bounds2& bounds) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    validate_bounds(bounds);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto result = std::make_unique<QueryData>();
    result->generation = state_->generation;
    if (state_->triangulation.dimension() != 2) return result;

    const IndexBox query_box(IndexPoint(bounds.xmin, bounds.ymin),
                             IndexPoint(bounds.xmax, bounds.ymax));
    const Kernel::Iso_rectangle_2 rectangle(bounds.xmin, bounds.ymin,
                                             bounds.xmax, bounds.ymax);
    std::vector<IndexValue> candidates;
    state_->face_index.query(bgi::intersects(query_box),
                             std::back_inserter(candidates));
    result->triangles.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto face = candidate.second;
        if (!finite_face(*state_, face)) continue;
        const Kernel::Triangle_2 triangle(face->vertex(0)->point(),
                                          face->vertex(1)->point(),
                                          face->vertex(2)->point());
        if (CGAL::do_intersect(rectangle, triangle)) {
            result->triangles.push_back(to_triangle(face));
        }
    }
    return result;
}

dt_statistics Context::statistics() const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    dt_statistics stats{};
    stats.struct_size = sizeof(stats);
    stats.dimension = static_cast<int32_t>(state_->triangulation.dimension());
    stats.vertex_count = state_->triangulation.number_of_vertices();
    stats.finite_triangle_count = state_->triangulation.number_of_faces();
    stats.generation = state_->generation;

    if (stats.vertex_count == 0) return stats;
    bool first = true;
    double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    for (auto it = state_->triangulation.finite_vertices_begin();
         it != state_->triangulation.finite_vertices_end(); ++it) {
        const double x = CGAL::to_double(it->point().x());
        const double y = CGAL::to_double(it->point().y());
        if (first) {
            xmin = xmax = x;
            ymin = ymax = y;
            first = false;
        } else {
            xmin = std::min(xmin, x);
            ymin = std::min(ymin, y);
            xmax = std::max(xmax, x);
            ymax = std::max(ymax, y);
        }
    }
    stats.bounds = {xmin, ymin, xmax, ymax};
    return stats;
}

bool Context::validate(bool verbose) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return state_->triangulation.is_valid(verbose);
}

void Context::save(const char* file_name) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    if (!file_name || *file_name == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "file name is empty");
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::ofstream stream(std::filesystem::u8path(file_name),
                         std::ios::binary | std::ios::trunc);
    if (!stream) throw Exception(DT_E_IO, "cannot open triangulation file for writing");

    FileHeader header{};
    std::memcpy(header.magic, kFileMagic, sizeof(kFileMagic));
    header.version = kFileVersion;
    header.header_size = sizeof(FileHeader);
    header.vertex_count = state_->triangulation.number_of_vertices();
    checked_write(stream, header);
    for (auto it = state_->triangulation.finite_vertices_begin();
         it != state_->triangulation.finite_vertices_end(); ++it) {
        FileVertex record{it->info().id, CGAL::to_double(it->point().x()),
                          CGAL::to_double(it->point().y()), it->info().z};
        checked_write(stream, record);
    }
}

dt_bounds2 Context::load(const char* file_name) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    if (!file_name || *file_name == '\0') {
        throw Exception(DT_E_INVALID_ARGUMENT, "file name is empty");
    }
    std::ifstream stream(std::filesystem::u8path(file_name), std::ios::binary);
    if (!stream) throw Exception(DT_E_IO, "cannot open triangulation file for reading");

    FileHeader header{};
    checked_read(stream, header);
    if (std::memcmp(header.magic, kFileMagic, sizeof(kFileMagic)) != 0 ||
        header.version != kFileVersion || header.header_size != sizeof(FileHeader) ||
        header.vertex_count > kMaxLoadVertices) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid or unsupported triangulation file");
    }

    std::vector<dt_point3> points(static_cast<size_t>(header.vertex_count));
    std::vector<dt_vertex_id> ids(static_cast<size_t>(header.vertex_count));
    for (uint64_t i = 0; i < header.vertex_count; ++i) {
        FileVertex record{};
        checked_read(stream, record);
        points[static_cast<size_t>(i)] = {record.x, record.y, record.z};
        ids[static_cast<size_t>(i)] = record.id;
    }
    auto next = make_state(points.data(), ids.data(), header.vertex_count, nullptr);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    next->generation = state_->generation + 1;
    state_.swap(next);
    lock.unlock();
    return statistics().bounds;
}

dt_bounds2 Context::import_points_text(const char* file_name) {
    require_file_name(file_name);
    std::ifstream stream(std::filesystem::u8path(file_name));
    if (!stream) throw Exception(DT_E_IO, "cannot open XYZ point file for reading");
    stream.imbue(std::locale::classic());

    std::vector<dt_point3> points;
    std::error_code size_error;
    const auto byte_count = std::filesystem::file_size(
        std::filesystem::u8path(file_name), size_error);
    if (!size_error) {
        const uint64_t estimate = std::min<uint64_t>(
            byte_count / 32 + 1, kMaxLoadVertices);
        points.reserve(static_cast<size_t>(estimate));
    }

    std::string line;
    uint64_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        dt_point3 point{};
        if (parse_point_text_line(line, line_number, point)) {
            if (points.size() >= kMaxLoadVertices) {
                throw Exception(DT_E_CORRUPTED_DATA,
                                "XYZ point file contains too many points");
            }
            points.push_back(point);
        }
    }
    if (stream.bad()) {
        throw Exception(DT_E_IO, "failed while reading XYZ point file");
    }
    if (points.empty()) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "XYZ point file contains no points");
    }

    build(points.data(), static_cast<uint64_t>(points.size()), nullptr);
    return statistics().bounds;
}

void Context::save_mesh_text(const char* file_name) const {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    require_file_name(file_name);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::ofstream stream(std::filesystem::u8path(file_name),
                         std::ios::out | std::ios::trunc);
    if (!stream) throw Exception(DT_E_IO, "cannot open text mesh file for writing");
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);

    stream << "DTMESH 1\n";
    stream << "VERTICES " << state_->triangulation.number_of_vertices() << '\n';
    for (auto it = state_->triangulation.finite_vertices_begin();
         it != state_->triangulation.finite_vertices_end(); ++it) {
        stream << it->info().id << ' ' << CGAL::to_double(it->point().x())
               << ' ' << CGAL::to_double(it->point().y()) << ' '
               << it->info().z << '\n';
    }

    stream << "TRIANGLES " << state_->triangulation.number_of_faces() << '\n';
    for (auto it = state_->triangulation.finite_faces_begin();
         it != state_->triangulation.finite_faces_end(); ++it) {
        stream << it->vertex(0)->info().id << ' '
               << it->vertex(1)->info().id << ' '
               << it->vertex(2)->info().id << '\n';
    }
    stream.flush();
    if (!stream) throw Exception(DT_E_IO, "failed while writing text mesh file");
}

dt_bounds2 Context::load_mesh_text(const char* file_name) {
    std::lock_guard<std::recursive_mutex> cgal_lock(g_cgal_mutex);
    require_file_name(file_name);
    std::ifstream stream(std::filesystem::u8path(file_name));
    if (!stream) throw Exception(DT_E_IO, "cannot open text mesh file for reading");
    stream.imbue(std::locale::classic());

    std::string token;
    uint32_t version = 0;
    uint64_t vertex_count = 0;
    if (!(stream >> token >> version) || token != "DTMESH" || version != 1 ||
        !(stream >> token >> vertex_count) || token != "VERTICES" ||
        vertex_count > kMaxLoadVertices) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "invalid or unsupported text mesh header");
    }

    std::vector<dt_point3> points(static_cast<size_t>(vertex_count));
    std::vector<dt_vertex_id> ids(static_cast<size_t>(vertex_count));
    for (uint64_t i = 0; i < vertex_count; ++i) {
        auto& point = points[static_cast<size_t>(i)];
        if (!(stream >> ids[static_cast<size_t>(i)] >> point.x >> point.y >> point.z)) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "truncated or invalid VERTICES section");
        }
        validate_point(point);
    }

    uint64_t triangle_count = 0;
    if (!(stream >> token >> triangle_count) || token != "TRIANGLES") {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "missing TRIANGLES section in text mesh");
    }

    auto next = make_state(points.data(), ids.data(), vertex_count, nullptr);
    if (triangle_count != next->triangulation.number_of_faces()) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "text mesh triangle count does not match its Delaunay vertices");
    }

    for (uint64_t i = 0; i < triangle_count; ++i) {
        dt_vertex_id id0 = 0, id1 = 0, id2 = 0;
        if (!(stream >> id0 >> id1 >> id2)) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "truncated or invalid TRIANGLES section");
        }
        const auto valid_id = [&](dt_vertex_id id) {
            return id != 0 && id < next->vertex_by_id.size() &&
                   next->vertex_by_id[static_cast<size_t>(id)] != VertexHandle();
        };
        if (!valid_id(id0) || !valid_id(id1) || !valid_id(id2) ||
            id0 == id1 || id1 == id2 || id0 == id2) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "triangle references an invalid vertex ID");
        }
        FaceHandle face;
        if (!next->triangulation.is_face(
                next->vertex_by_id[static_cast<size_t>(id0)],
                next->vertex_by_id[static_cast<size_t>(id1)],
                next->vertex_by_id[static_cast<size_t>(id2)], face) ||
            !finite_face(*next, face)) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "text mesh contains a non-Delaunay triangle");
        }
        if (face->info().reserved != 0) {
            throw Exception(DT_E_CORRUPTED_DATA,
                            "text mesh contains a duplicate triangle");
        }
        face->info().reserved = 1;
    }
    if (stream >> token) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "unexpected data after TRIANGLES section");
    }
    for (auto it = next->triangulation.finite_faces_begin();
         it != next->triangulation.finite_faces_end(); ++it) {
        it->info().reserved = 0;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    next->generation = state_->generation + 1;
    state_.swap(next);
    lock.unlock();
    return statistics().bounds;
}

} // namespace dt
