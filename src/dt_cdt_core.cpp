#include "dt_cdt_core.hpp"
#include "dt_surface_analysis.hpp"

#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Constrained_triangulation_face_base_2.h>
#include <CGAL/Intersections_2/Iso_rectangle_2_Triangle_2.h>
#include <CGAL/Triangulation_data_structure_2.h>
#include <CGAL/Triangulation_face_base_with_info_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
#include <CGAL/number_utils.h>

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <cstring>
#include <cstdint>
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
using PointKey = std::tuple<double, double, double>;
using TriangleKey = std::array<PointKey, 3>;
using GeometryEdgeKey = std::pair<PointKey, PointKey>;
namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;
using IndexPoint = bg::model::point<double, 2, bg::cs::cartesian>;
using IndexBox = bg::model::box<IndexPoint>;

struct ConstraintSegmentRef {
    size_t constraint_index = 0;
    size_t segment_index = 0;
    dt_point3 a{};
    dt_point3 b{};
};

struct SegmentCut {
    double t = 0.0;
    dt_point3 point{};
};

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

bool valid_constraint_kind(int32_t kind) {
    return kind == DT_CONSTRAINT_BREAKLINE ||
           kind == DT_CONSTRAINT_OUTER_BOUNDARY ||
           kind == DT_CONSTRAINT_HOLE_BOUNDARY;
}

bool valid_crossing_policy(int32_t policy) {
    return policy == DT_CDT_CROSSING_REJECT ||
           policy == DT_CDT_CROSSING_SPLIT_COMPATIBLE_Z;
}

std::vector<CdtConstraint> node_constraint_crossings(
    const std::vector<dt_point3>& base_points,
    const std::vector<CdtConstraint>& input, double z_tolerance) {
    std::vector<CdtConstraint> result = input;
    std::vector<ConstraintSegmentRef> segments;
    std::vector<std::pair<IndexBox, size_t>> indexed;
    for (size_t ci = 0; ci < input.size(); ++ci) {
        const auto& constraint = input[ci];
        const size_t count = constraint.points.size();
        if (count < 2) continue;
        const size_t segment_count = count - 1 +
            ((constraint.flags & DT_CONSTRAINT_CLOSED) ? 1 : 0);
        for (size_t si = 0; si < segment_count; ++si) {
            const auto a = constraint.points[si % count];
            const auto b = constraint.points[(si + 1) % count];
            const size_t index = segments.size();
            segments.push_back({ci, si, a, b});
            indexed.push_back({IndexBox(
                IndexPoint(std::min(a.x, b.x), std::min(a.y, b.y)),
                IndexPoint(std::max(a.x, b.x), std::max(a.y, b.y))), index});
        }
    }

    bgi::rtree<std::pair<IndexBox, size_t>, bgi::quadratic<16>> tree(
        indexed.begin(), indexed.end());
    std::vector<std::vector<SegmentCut>> cuts(segments.size());
    std::map<std::pair<double, double>, double> base_z;
    for (const auto& point : base_points)
        base_z.emplace(std::make_pair(point.x, point.y), point.z);

    constexpr double endpoint_epsilon = 1e-12;
    for (size_t i = 0; i < segments.size(); ++i) {
        std::vector<std::pair<IndexBox, size_t>> candidates;
        tree.query(bgi::intersects(indexed[i].first),
                   std::back_inserter(candidates));
        const auto& lhs = segments[i];
        const Kernel::Segment_2 left(Kernel::Point_2(lhs.a.x, lhs.a.y),
                                     Kernel::Point_2(lhs.b.x, lhs.b.y));
        for (const auto& candidate : candidates) {
            const size_t j = candidate.second;
            if (j <= i) continue;
            const auto& rhs = segments[j];
            const Kernel::Segment_2 right(Kernel::Point_2(rhs.a.x, rhs.a.y),
                                          Kernel::Point_2(rhs.b.x, rhs.b.y));
            if (!CGAL::do_intersect(left, right)) continue;

            const double rx = lhs.b.x - lhs.a.x;
            const double ry = lhs.b.y - lhs.a.y;
            const double sx = rhs.b.x - rhs.a.x;
            const double sy = rhs.b.y - rhs.a.y;
            const double denominator = rx * sy - ry * sx;
            const double scale = std::max({1.0, std::abs(rx), std::abs(ry),
                                           std::abs(sx), std::abs(sy)});
            if (std::abs(denominator) <=
                32.0 * std::numeric_limits<double>::epsilon() * scale * scale) {
                const bool shared_endpoint =
                    same_xy(lhs.a, rhs.a) || same_xy(lhs.a, rhs.b) ||
                    same_xy(lhs.b, rhs.a) || same_xy(lhs.b, rhs.b);
                const auto interior = [](const Kernel::Segment_2& segment,
                                         const Kernel::Point_2& point) {
                    return segment.has_on(point) && point != segment.source() &&
                           point != segment.target();
                };
                if (!shared_endpoint || interior(left, right.source()) ||
                    interior(left, right.target()) ||
                    interior(right, left.source()) ||
                    interior(right, left.target())) {
                    throw Exception(DT_E_UNSUPPORTED,
                                    "overlapping constraint segments are not supported");
                }
                continue;
            }

            const double qpx = rhs.a.x - lhs.a.x;
            const double qpy = rhs.a.y - lhs.a.y;
            double t = (qpx * sy - qpy * sx) / denominator;
            double u = (qpx * ry - qpy * rx) / denominator;
            t = std::clamp(t, 0.0, 1.0);
            u = std::clamp(u, 0.0, 1.0);
            const bool lhs_interior = t > endpoint_epsilon &&
                                      t < 1.0 - endpoint_epsilon;
            const bool rhs_interior = u > endpoint_epsilon &&
                                      u < 1.0 - endpoint_epsilon;
            if (!lhs_interior && !rhs_interior) continue;

            dt_point3 point{lhs.a.x + t * rx, lhs.a.y + t * ry, 0.0};
            const double lhs_z = lhs.a.z + t * (lhs.b.z - lhs.a.z);
            const double rhs_z = rhs.a.z + u * (rhs.b.z - rhs.a.z);
            if (std::abs(lhs_z - rhs_z) > z_tolerance) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "crossing constraint Z values exceed tolerance");
            }
            point.z = 0.5 * (lhs_z + rhs_z);
            if (!lhs_interior) point = t <= endpoint_epsilon ? lhs.a : lhs.b;
            if (!rhs_interior) point = u <= endpoint_epsilon ? rhs.a : rhs.b;
            const auto base = base_z.find({point.x, point.y});
            if (base != base_z.end()) {
                if (std::abs(base->second - point.z) > z_tolerance) {
                    throw Exception(DT_E_INVALID_ARGUMENT,
                                    "crossing Z conflicts with a base point");
                }
                point.z = base->second;
            }
            if (lhs_interior) cuts[i].push_back({t, point});
            if (rhs_interior) cuts[j].push_back({u, point});
        }
    }

    std::vector<std::vector<std::vector<SegmentCut>>> grouped(result.size());
    for (size_t ci = 0; ci < result.size(); ++ci) {
        const size_t n = result[ci].points.size();
        grouped[ci].resize(n - 1 +
            ((result[ci].flags & DT_CONSTRAINT_CLOSED) ? 1 : 0));
    }
    for (size_t i = 0; i < segments.size(); ++i)
        grouped[segments[i].constraint_index][segments[i].segment_index] =
            std::move(cuts[i]);

    for (size_t ci = 0; ci < result.size(); ++ci) {
        const auto original = result[ci].points;
        const size_t n = original.size();
        const bool closed = (result[ci].flags & DT_CONSTRAINT_CLOSED) != 0;
        std::vector<dt_point3> noded;
        noded.reserve(n);
        const size_t segment_count = n - 1 + (closed ? 1 : 0);
        for (size_t si = 0; si < segment_count; ++si) {
            if (si == 0) noded.push_back(original[0]);
            auto& segment_cuts = grouped[ci][si];
            std::sort(segment_cuts.begin(), segment_cuts.end(),
                      [](const SegmentCut& a, const SegmentCut& b) {
                          return a.t < b.t;
                      });
            for (const auto& cut : segment_cuts) {
                if (!same_xy(noded.back(), cut.point)) noded.push_back(cut.point);
            }
            const auto& end = original[(si + 1) % n];
            if (!closed || si + 1 < segment_count) {
                if (!same_xy(noded.back(), end)) noded.push_back(end);
            }
        }
        result[ci].points = std::move(noded);
    }
    return result;
}

void assign_constraint_geometry(CdtConstraint& constraint, uint32_t flags,
                                const dt_point3* points, uint64_t count) {
    if (!points || count == 0) {
        throw Exception(DT_E_INVALID_ARGUMENT, "constraint points is empty");
    }
    if (count > kMaxLoadConstraintPoints) {
        throw Exception(DT_E_LIMIT_EXCEEDED,
                        "too many constraint points");
    }
    constraint.flags = flags & DT_CONSTRAINT_CLOSED;
    if (constraint.kind != DT_CONSTRAINT_BREAKLINE)
        constraint.flags |= DT_CONSTRAINT_CLOSED;
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

PointKey point_key(const dt_point3& point) {
    return {point.x, point.y, point.z};
}

TriangleKey triangle_key(const dt_triangle3& triangle) {
    TriangleKey key{point_key(triangle.vertex[0].point),
                    point_key(triangle.vertex[1].point),
                    point_key(triangle.vertex[2].point)};
    std::sort(key.begin(), key.end());
    return key;
}

GeometryEdgeKey geometry_edge_key(const dt_segment3& segment) {
    auto a = point_key(segment.vertex[0].point);
    auto b = point_key(segment.vertex[1].point);
    if (b < a) std::swap(a, b);
    return {a, b};
}

dt_segment3 triangle_edge(const dt_triangle3& triangle, size_t index) {
    return {{triangle.vertex[index], triangle.vertex[(index + 1) % 3]}};
}

double interpolate_edge_z(CdtVertexHandle a, CdtVertexHandle b,
                          const dt_point3& query) {
    const double ax = CGAL::to_double(a->point().x());
    const double ay = CGAL::to_double(a->point().y());
    const double bx = CGAL::to_double(b->point().x());
    const double by = CGAL::to_double(b->point().y());
    const double dx = bx - ax;
    const double dy = by - ay;
    const double denominator = dx * dx + dy * dy;
    if (denominator == 0.0) return a->info().z;
    const double t = std::clamp(((query.x - ax) * dx + (query.y - ay) * dy) /
                                    denominator,
                                0.0, 1.0);
    return a->info().z + t * (b->info().z - a->info().z);
}

double interpolate_face_z(CdtFaceHandle face, const dt_point3& query) {
    const auto a = to_vertex(face->vertex(0)).point;
    const auto b = to_vertex(face->vertex(1)).point;
    const auto c = to_vertex(face->vertex(2)).point;
    const double denominator =
        (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (denominator == 0.0) {
        throw Exception(DT_E_INTERNAL, "degenerate CDT triangle");
    }
    const double wa =
        ((b.y - c.y) * (query.x - c.x) +
         (c.x - b.x) * (query.y - c.y)) /
        denominator;
    const double wb =
        ((c.y - a.y) * (query.x - c.x) +
         (a.x - c.x) * (query.y - c.y)) /
        denominator;
    return wa * a.z + wb * b.z + (1.0 - wa - wb) * c.z;
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

constexpr size_t kCdtBinaryHeaderSize = 4096;
constexpr size_t kCdtBinaryCrsOffset = 256;
constexpr size_t kCdtBinaryDirectoryRecordSize = 64;
constexpr uint32_t kCdtBinaryVersion = 1;
constexpr uint32_t kCdtBinaryEndian = 0x01020304U;
constexpr char kCdtBinaryMagic[8] = {'D', 'C', 'D', 'T', 'B', '1', '\0', '\0'};

template <class T>
void binary_store(std::array<unsigned char, kCdtBinaryHeaderSize>& header,
                  size_t offset, const T& value) {
    if (offset > header.size() || sizeof(T) > header.size() - offset)
        throw Exception(DT_E_INTERNAL, "DCDTB header overflow");
    std::memcpy(header.data() + offset, &value, sizeof(T));
}

template <class T>
T binary_load(const std::array<unsigned char, kCdtBinaryHeaderSize>& header,
              size_t offset) {
    if (offset > header.size() || sizeof(T) > header.size() - offset)
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DCDTB header");
    T value{};
    std::memcpy(&value, header.data() + offset, sizeof(T));
    return value;
}

uint64_t checked_add(uint64_t a, uint64_t b, const char* message) {
    if (b > std::numeric_limits<uint64_t>::max() - a)
        throw Exception(DT_E_LIMIT_EXCEEDED, message);
    return a + b;
}

uint64_t checked_mul(uint64_t a, uint64_t b, const char* message) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        throw Exception(DT_E_LIMIT_EXCEEDED, message);
    return a * b;
}

uint64_t fnv_update(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct BinaryCdtMetadata {
    uint64_t file_size = 0;
    uint64_t base_count = 0;
    uint64_t constraint_count = 0;
    uint64_t next_constraint_id = 1;
    uint64_t base_offset = 0;
    uint64_t directory_offset = 0;
    uint64_t constraint_points_offset = 0;
    uint64_t crs_length = 0;
    uint64_t payload_hash = 0;
    uint64_t total_constraint_points = 0;
    std::string crs;
};

BinaryCdtMetadata read_binary_metadata(std::ifstream& input) {
    std::array<unsigned char, kCdtBinaryHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (!input) throw Exception(DT_E_CORRUPTED_DATA, "truncated DCDTB header");
    if (std::memcmp(header.data(), kCdtBinaryMagic, sizeof(kCdtBinaryMagic)) != 0 ||
        binary_load<uint32_t>(header, 8) != kCdtBinaryVersion ||
        binary_load<uint32_t>(header, 12) != kCdtBinaryEndian ||
        binary_load<uint64_t>(header, 16) != kCdtBinaryHeaderSize) {
        throw Exception(DT_E_CORRUPTED_DATA, "unsupported DCDTB header");
    }
    BinaryCdtMetadata metadata;
    metadata.file_size = binary_load<uint64_t>(header, 24);
    metadata.base_count = binary_load<uint64_t>(header, 32);
    metadata.constraint_count = binary_load<uint64_t>(header, 40);
    metadata.next_constraint_id = binary_load<uint64_t>(header, 48);
    metadata.base_offset = binary_load<uint64_t>(header, 56);
    metadata.directory_offset = binary_load<uint64_t>(header, 64);
    metadata.constraint_points_offset = binary_load<uint64_t>(header, 72);
    metadata.crs_length = binary_load<uint64_t>(header, 80);
    metadata.payload_hash = binary_load<uint64_t>(header, 88);
    metadata.total_constraint_points = binary_load<uint64_t>(header, 96);
    if (metadata.base_count > kMaxLoadPoints ||
        metadata.constraint_count > kMaxLoadConstraints ||
        metadata.total_constraint_points > kMaxLoadConstraintPoints ||
        metadata.crs_length > kCdtBinaryHeaderSize - kCdtBinaryCrsOffset ||
        metadata.base_offset != kCdtBinaryHeaderSize) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DCDTB metadata limits");
    }
    const uint64_t base_bytes = checked_mul(metadata.base_count,
                                            sizeof(dt_point3),
                                            "DCDTB base point array is too large");
    const uint64_t directory_bytes = checked_mul(
        metadata.constraint_count, kCdtBinaryDirectoryRecordSize,
        "DCDTB directory is too large");
    const uint64_t constraint_bytes = checked_mul(
        metadata.total_constraint_points, sizeof(dt_point3),
        "DCDTB constraint point array is too large");
    if (metadata.directory_offset != metadata.base_offset + base_bytes ||
        metadata.constraint_points_offset !=
            metadata.directory_offset + directory_bytes ||
        metadata.file_size != metadata.constraint_points_offset +
                                  constraint_bytes) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DCDTB section offsets");
    }
    input.seekg(0, std::ios::end);
    const auto actual_size = input.tellg();
    if (actual_size < 0 || static_cast<uint64_t>(actual_size) != metadata.file_size)
        throw Exception(DT_E_CORRUPTED_DATA, "DCDTB file size mismatch");
    metadata.crs.assign(
        reinterpret_cast<const char*>(header.data() + kCdtBinaryCrsOffset),
        static_cast<size_t>(metadata.crs_length));
    return metadata;
}

dt_cdt_binary_index_entry read_binary_directory_entry(std::ifstream& input) {
    std::array<unsigned char, kCdtBinaryDirectoryRecordSize> record{};
    input.read(reinterpret_cast<char*>(record.data()),
               static_cast<std::streamsize>(record.size()));
    if (!input)
        throw Exception(DT_E_CORRUPTED_DATA, "truncated DCDTB directory");
    dt_cdt_binary_index_entry entry{};
    entry.struct_size = sizeof(entry);
    std::memcpy(&entry.id, record.data(), sizeof(entry.id));
    std::memcpy(&entry.kind, record.data() + 8, sizeof(entry.kind));
    std::memcpy(&entry.flags, record.data() + 12, sizeof(entry.flags));
    std::memcpy(&entry.point_count, record.data() + 16,
                sizeof(entry.point_count));
    std::memcpy(&entry.point_offset, record.data() + 24,
                sizeof(entry.point_offset));
    std::memcpy(&entry.bounds, record.data() + 32, sizeof(entry.bounds));
    if (entry.id == 0 || !valid_constraint_kind(entry.kind) ||
        (entry.flags & ~DT_CONSTRAINT_CLOSED) != 0 ||
        entry.point_count < ((entry.flags & DT_CONSTRAINT_CLOSED) ? 3u : 2u) ||
        entry.point_count > kMaxLoadConstraintPoints ||
        !finite_value(entry.bounds.xmin) || !finite_value(entry.bounds.ymin) ||
        !finite_value(entry.bounds.xmax) || !finite_value(entry.bounds.ymax) ||
        entry.bounds.xmin > entry.bounds.xmax ||
        entry.bounds.ymin > entry.bounds.ymax) {
        throw Exception(DT_E_CORRUPTED_DATA, "invalid DCDTB directory entry");
    }
    return entry;
}

bool bounds_intersect(const dt_bounds2& a, const dt_bounds2& b) {
    return a.xmin <= b.xmax && a.xmax >= b.xmin &&
           a.ymin <= b.ymax && a.ymax >= b.ymin;
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
    dt_vertex_id next_vertex_id = 1;
    bool has_outer_boundary = false;
    dt_bounds2 bounds{};
    std::string crs_wkt;
};

CdtContext::CdtContext(const dt_cdt_options* options)
    : state_(std::make_unique<State>()) {
    if (options) {
        if (!valid_crossing_policy(options->crossing_policy) ||
            !finite_value(options->crossing_z_tolerance) ||
            options->crossing_z_tolerance < 0.0) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "invalid CDT crossing policy options");
        }
        crossing_policy_ = options->crossing_policy;
        crossing_z_tolerance_ = options->crossing_z_tolerance;
    }
}
CdtContext::~CdtContext() = default;

std::unique_ptr<CdtContext::State> CdtContext::make_state(
    const std::vector<dt_point3>& base_points,
    const std::vector<CdtConstraint>& constraints,
    dt_constraint_id next_constraint_id, uint64_t generation,
    const std::string& crs_wkt, int32_t crossing_policy,
    double crossing_z_tolerance) {
    auto next = std::make_unique<State>();
    next->base_points = base_points;
    next->constraints = crossing_policy == DT_CDT_CROSSING_SPLIT_COMPATIBLE_Z
        ? node_constraint_crossings(base_points, constraints,
                                    crossing_z_tolerance)
        : constraints;
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
    for (const auto& constraint : next->constraints) {
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
        for (const auto& constraint : next->constraints) {
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
    for (const auto& constraint : next->constraints) {
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
    if (next->triangulation.dimension() == 2) {
        mark_domains(*next);
        for (auto face = next->triangulation.finite_faces_begin();
             face != next->triangulation.finite_faces_end(); ++face) {
            if (domain_face(*next, face)) ++next->domain_triangle_count;
        }
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
    next->next_vertex_id = next_vertex_id;
    return next;
}

std::unique_ptr<CdtContext::State> CdtContext::make_local_add_state(
    const State& current, const CdtConstraint& constraint,
    uint64_t generation) {
    const bool closed = (constraint.flags & DT_CONSTRAINT_CLOSED) != 0;
    if (constraint.points.size() < (closed ? 3u : 2u)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "constraint has too few distinct points");
    }
    for (size_t i = 1; i < constraint.points.size(); ++i) {
        if (same_xy(constraint.points[i - 1], constraint.points[i])) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "constraint contains a zero-length segment");
        }
    }
    auto next = std::make_unique<State>(current);
    const bool had_vertices = current.triangulation.number_of_vertices() != 0;
    next->generation = generation;
    for (const auto& point : constraint.points) {
        validate_point(point);
        const auto handle = next->triangulation.insert(
            Kernel::Point_2(point.x, point.y));
        if (handle->info().id == 0) {
            handle->info() = VertexInfo{next->next_vertex_id++, point.z};
        } else if (!compatible_z(handle->info().z, point.z)) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "shared constraint point has inconsistent Z");
        }
    }
    try {
        const size_t count = constraint.points.size();
        const size_t segment_count = count - 1 +
            ((constraint.flags & DT_CONSTRAINT_CLOSED) ? 1 : 0);
        for (size_t i = 0; i < segment_count; ++i) {
            const auto& a = constraint.points[i % count];
            const auto& b = constraint.points[(i + 1) % count];
            const auto va = next->triangulation.insert(Kernel::Point_2(a.x, a.y));
            const auto vb = next->triangulation.insert(Kernel::Point_2(b.x, b.y));
            next->triangulation.insert_constraint(va, vb);
        }
    } catch (const Cdt::Intersection_of_constraints_exception&) {
        throw Exception(DT_E_UNSUPPORTED,
                        "intersecting constraint segments are not supported; "
                        "enable the split crossing policy or pre-node them");
    }
    next->constraints.push_back(constraint);
    next->constrained_edge_count = 0;
    for (auto edge = next->triangulation.constrained_edges_begin();
         edge != next->triangulation.constrained_edges_end(); ++edge)
        ++next->constrained_edge_count;
    next->domain_triangle_count = 0;
    if (next->triangulation.dimension() == 2) {
        mark_domains(*next);
        for (auto face = next->triangulation.finite_faces_begin();
             face != next->triangulation.finite_faces_end(); ++face) {
            if (domain_face(*next, face)) ++next->domain_triangle_count;
        }
    }
    bool first = !had_vertices;
    for (const auto& point : constraint.points) {
        if (first) {
            next->bounds = {point.x, point.y, point.x, point.y};
            first = false;
        } else {
            next->bounds.xmin = std::min(next->bounds.xmin, point.x);
            next->bounds.ymin = std::min(next->bounds.ymin, point.y);
            next->bounds.xmax = std::max(next->bounds.xmax, point.x);
            next->bounds.ymax = std::max(next->bounds.ymax, point.y);
        }
    }
    return next;
}

std::unique_ptr<EditData> CdtContext::make_edit_data(
    const State& before, const State& after) {
    auto result = std::make_unique<EditData>();
    result->generation = after.generation;

    std::map<TriangleKey, dt_triangle3> old_triangles;
    std::map<TriangleKey, dt_triangle3> new_triangles;
    for (auto face = before.triangulation.finite_faces_begin();
         face != before.triangulation.finite_faces_end(); ++face) {
        if (!domain_face(before, face)) continue;
        const auto triangle = to_triangle(face);
        old_triangles.emplace(triangle_key(triangle), triangle);
    }
    for (auto face = after.triangulation.finite_faces_begin();
         face != after.triangulation.finite_faces_end(); ++face) {
        if (!domain_face(after, face)) continue;
        const auto triangle = to_triangle(face);
        new_triangles.emplace(triangle_key(triangle), triangle);
    }
    for (const auto& item : old_triangles) {
        if (new_triangles.count(item.first) == 0)
            result->removed_triangles.push_back(item.second);
    }
    for (const auto& item : new_triangles) {
        if (old_triangles.count(item.first) == 0)
            result->added_triangles.push_back(item.second);
    }

    auto collect_edges = [](const std::vector<dt_triangle3>& triangles,
                            std::vector<dt_segment3>& edges,
                            std::vector<dt_segment3>* boundary) {
        std::map<GeometryEdgeKey, std::pair<dt_segment3, uint64_t>> unique;
        for (const auto& triangle : triangles) {
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto segment = triangle_edge(triangle, edge);
                auto& entry = unique[geometry_edge_key(segment)];
                if (entry.second == 0) entry.first = segment;
                ++entry.second;
            }
        }
        edges.reserve(unique.size());
        for (const auto& item : unique) {
            edges.push_back(item.second.first);
            if (boundary && item.second.second == 1)
                boundary->push_back(item.second.first);
        }
    };
    collect_edges(result->removed_triangles, result->removed_edges,
                  &result->boundary_edges);
    collect_edges(result->added_triangles, result->added_edges, nullptr);
    return result;
}

void CdtContext::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    state_ = make_state({}, {}, 1, state_->generation + 1, state_->crs_wkt,
                        crossing_policy_, crossing_z_tolerance_);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
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
                           state_->crs_wkt, crossing_policy_,
                           crossing_z_tolerance_);
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
}

void CdtContext::build_from_tin(std::vector<dt_point3> points,
                                std::string crs_wkt) {
    if (points.size() > kMaxLoadPoints) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "too many CDT base points");
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto next = make_state(points, {}, 1, state_->generation + 1, crs_wkt,
                           crossing_policy_, crossing_z_tolerance_);
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
}

dt_constraint_id CdtContext::add_constraint(int32_t kind, uint32_t flags,
                                             const dt_point3* points,
                                             uint64_t count) {
    if (!valid_constraint_kind(kind)) {
        throw Exception(DT_E_INVALID_ARGUMENT, "unknown constraint kind");
    }
    CdtConstraint constraint;
    constraint.kind = kind;
    assign_constraint_geometry(constraint, flags, points, count);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (state_->constraints.size() >= kMaxLoadConstraints) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "too many CDT constraints");
    }
    constraint.id = state_->next_constraint_id;
    if (constraint.id == 0 || constraint.id ==
                                  std::numeric_limits<dt_constraint_id>::max()) {
        throw Exception(DT_E_LIMIT_EXCEEDED, "constraint id space exhausted");
    }
    std::unique_ptr<State> next;
    if (kind == DT_CONSTRAINT_BREAKLINE &&
        crossing_policy_ == DT_CDT_CROSSING_REJECT) {
        next = make_local_add_state(*state_, constraint,
                                    state_->generation + 1);
        next->next_constraint_id = constraint.id + 1;
    } else {
        auto constraints = state_->constraints;
        constraints.push_back(constraint);
        next = make_state(state_->base_points, constraints, constraint.id + 1,
                          state_->generation + 1, state_->crs_wkt,
                          crossing_policy_, crossing_z_tolerance_);
    }
    state_.swap(next);
    if (kind == DT_CONSTRAINT_BREAKLINE &&
        crossing_policy_ == DT_CDT_CROSSING_REJECT) {
        ++local_topology_edit_count_;
        last_edit_mode_ = DT_CDT_EDIT_MODE_LOCAL_TOPOLOGY;
    } else {
        ++full_rebuild_count_;
        last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    }
    return constraint.id;
}

std::unique_ptr<EditData> CdtContext::update_constraint(
    dt_constraint_id id, uint32_t flags, const dt_point3* points,
    uint64_t count, bool capture_effect) {
    if (id == 0) throw Exception(DT_E_INVALID_ARGUMENT, "constraint id is zero");
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto constraints = state_->constraints;
    const auto found = std::find_if(
        constraints.begin(), constraints.end(),
        [id](const CdtConstraint& constraint) { return constraint.id == id; });
    if (found == constraints.end()) {
        throw Exception(DT_E_NOT_FOUND, "constraint id was not found");
    }

    assign_constraint_geometry(*found, flags, points, count);

    auto next = make_state(state_->base_points, constraints,
                           state_->next_constraint_id, state_->generation + 1,
                           state_->crs_wkt, crossing_policy_,
                           crossing_z_tolerance_);
    auto effect = capture_effect ? make_edit_data(*state_, *next) : nullptr;
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    return effect;
}

dt_cdt_vertex_usage CdtContext::constraint_vertex_usage(
    dt_constraint_id id, uint64_t point_index) const {
    if (id == 0) throw Exception(DT_E_INVALID_ARGUMENT, "constraint id is zero");
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = std::find_if(
        state_->constraints.begin(), state_->constraints.end(),
        [id](const CdtConstraint& constraint) { return constraint.id == id; });
    if (found == state_->constraints.end()) {
        throw Exception(DT_E_NOT_FOUND, "constraint id was not found");
    }
    if (point_index >= found->points.size()) {
        throw Exception(DT_E_NOT_FOUND,
                        "constraint point index is out of range");
    }

    const auto& point = found->points[static_cast<size_t>(point_index)];
    dt_cdt_vertex_usage result{};
    result.struct_size = sizeof(result);
    result.point = point;
    for (const auto& constraint : state_->constraints) {
        bool referenced_by_constraint = false;
        for (const auto& candidate : constraint.points) {
            if (!same_xy(candidate, point)) continue;
            ++result.reference_count;
            referenced_by_constraint = true;
        }
        if (referenced_by_constraint) ++result.constraint_count;
    }
    result.is_base_point = std::any_of(
        state_->base_points.begin(), state_->base_points.end(),
        [&](const dt_point3& candidate) { return same_xy(candidate, point); })
                               ? 1u
                               : 0u;
    return result;
}

std::unique_ptr<EditData> CdtContext::remove_constraint_vertex(
    dt_constraint_id id, uint64_t point_index, uint32_t flags,
    bool capture_effect) {
    if (id == 0) throw Exception(DT_E_INVALID_ARGUMENT, "constraint id is zero");
    if ((flags & ~DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH) != 0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "unknown constraint vertex removal flags");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto constraints = state_->constraints;
    const auto found = std::find_if(
        constraints.begin(), constraints.end(),
        [id](const CdtConstraint& constraint) { return constraint.id == id; });
    if (found == constraints.end()) {
        throw Exception(DT_E_NOT_FOUND, "constraint id was not found");
    }
    if (point_index >= found->points.size()) {
        throw Exception(DT_E_NOT_FOUND,
                        "constraint point index is out of range");
    }

    const auto point = found->points[static_cast<size_t>(point_index)];
    uint64_t constraint_count = 0;
    for (const auto& constraint : constraints) {
        if (std::any_of(constraint.points.begin(), constraint.points.end(),
                        [&](const dt_point3& candidate) {
                            return same_xy(candidate, point);
                        })) {
            ++constraint_count;
        }
    }
    if (constraint_count > 1 &&
        (flags & DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH) == 0) {
        throw Exception(
            DT_E_UNSUPPORTED,
            "constraint vertex is shared; explicitly allow detaching only "
            "the selected constraint reference");
    }

    found->points.erase(found->points.begin() +
                        static_cast<size_t>(point_index));
    auto next = make_state(state_->base_points, constraints,
                           state_->next_constraint_id, state_->generation + 1,
                           state_->crs_wkt, crossing_policy_,
                           crossing_z_tolerance_);
    auto effect = capture_effect ? make_edit_data(*state_, *next) : nullptr;
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    return effect;
}

std::unique_ptr<EditData> CdtContext::apply_constraint_edits(
    const dt_cdt_constraint_edit* edits, uint64_t edit_count,
    std::vector<dt_constraint_id>& result_ids, bool capture_effect) {
    if (!edits || edit_count == 0) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "constraint edit batch is empty");
    }
    if (edit_count > kMaxLoadConstraints) {
        throw Exception(DT_E_LIMIT_EXCEEDED,
                        "too many constraint edits");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto constraints = state_->constraints;
    auto next_constraint_id = state_->next_constraint_id;
    std::vector<dt_constraint_id> ids(static_cast<size_t>(edit_count), 0);

    for (uint64_t index = 0; index < edit_count; ++index) {
        const auto& edit = edits[static_cast<size_t>(index)];
        if (edit.struct_size != sizeof(dt_cdt_constraint_edit)) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "constraint edit struct_size is invalid");
        }
        if (edit.operation == DT_CDT_EDIT_ADD) {
            if (edit.constraint_id != 0 || !valid_constraint_kind(edit.kind)) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "invalid add constraint edit");
            }
            if (next_constraint_id == 0 ||
                next_constraint_id ==
                    std::numeric_limits<dt_constraint_id>::max()) {
                throw Exception(DT_E_LIMIT_EXCEEDED,
                                "constraint id space exhausted");
            }
            CdtConstraint constraint;
            constraint.id = next_constraint_id++;
            constraint.kind = edit.kind;
            assign_constraint_geometry(constraint, edit.flags, edit.points,
                                       edit.point_count);
            constraints.push_back(std::move(constraint));
            if (constraints.size() > kMaxLoadConstraints) {
                throw Exception(DT_E_LIMIT_EXCEEDED,
                                "too many CDT constraints");
            }
            ids[static_cast<size_t>(index)] = constraints.back().id;
            continue;
        }

        if (edit.constraint_id == 0 || edit.kind != 0) {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "update or remove edit requires an existing id "
                            "and kind zero");
        }
        const auto found = std::find_if(
            constraints.begin(), constraints.end(),
            [&](const CdtConstraint& constraint) {
                return constraint.id == edit.constraint_id;
            });
        if (found == constraints.end()) {
            throw Exception(DT_E_NOT_FOUND,
                            "constraint edit id was not found");
        }
        ids[static_cast<size_t>(index)] = edit.constraint_id;
        if (edit.operation == DT_CDT_EDIT_UPDATE) {
            assign_constraint_geometry(*found, edit.flags, edit.points,
                                       edit.point_count);
        } else if (edit.operation == DT_CDT_EDIT_REMOVE) {
            if (edit.flags != 0 || edit.points != nullptr ||
                edit.point_count != 0) {
                throw Exception(DT_E_INVALID_ARGUMENT,
                                "remove constraint edit must not contain "
                                "geometry or flags");
            }
            constraints.erase(found);
        } else {
            throw Exception(DT_E_INVALID_ARGUMENT,
                            "unknown constraint edit operation");
        }
    }

    auto next = make_state(state_->base_points, constraints,
                           next_constraint_id, state_->generation + 1,
                           state_->crs_wkt, crossing_policy_,
                           crossing_z_tolerance_);
    auto effect = capture_effect ? make_edit_data(*state_, *next) : nullptr;
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    result_ids = std::move(ids);
    return effect;
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
                           state_->crs_wkt, crossing_policy_,
                           crossing_z_tolerance_);
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
}

void CdtContext::set_crossing_policy(int32_t policy, double z_tolerance) {
    if (!valid_crossing_policy(policy) || !finite_value(z_tolerance) ||
        z_tolerance < 0.0) {
        throw Exception(DT_E_INVALID_ARGUMENT, "invalid CDT crossing policy");
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    crossing_policy_ = policy;
    crossing_z_tolerance_ = z_tolerance;
}

dt_cdt_edit_metrics CdtContext::edit_metrics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    dt_cdt_edit_metrics result{};
    result.struct_size = sizeof(result);
    result.last_edit_mode = last_edit_mode_;
    result.local_topology_edit_count = local_topology_edit_count_;
    result.full_rebuild_count = full_rebuild_count_;
    result.generation = state_->generation;
    return result;
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

double CdtContext::sample_height_xy(const dt_point3& query_point) const {
    validate_point(query_point);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (state_->triangulation.dimension() != 2) {
        throw Exception(DT_E_EMPTY, "CDT has no two-dimensional surface");
    }
    Cdt::Locate_type type;
    int index = 0;
    const auto face = state_->triangulation.locate(
        Kernel::Point_2(query_point.x, query_point.y), type, index);
    if (type == Cdt::FACE) {
        if (domain_face(*state_, face))
            return interpolate_face_z(face, query_point);
    } else if (type == Cdt::EDGE) {
        const auto neighbor = face->neighbor(index);
        if (domain_face(*state_, face) || domain_face(*state_, neighbor)) {
            const auto vertices = edge_vertices(CdtEdge(face, index));
            if (!state_->triangulation.is_infinite(vertices.first) &&
                !state_->triangulation.is_infinite(vertices.second)) {
                return interpolate_edge_z(vertices.first, vertices.second,
                                          query_point);
            }
        }
    } else if (type == Cdt::VERTEX) {
        const auto vertex = face->vertex(index);
        auto incident = state_->triangulation.incident_faces(vertex);
        if (incident != 0) {
            const auto begin = incident;
            do {
                if (domain_face(*state_, incident)) return vertex->info().z;
                ++incident;
            } while (incident != begin);
        }
    }
    throw Exception(DT_E_NOT_FOUND,
                    "query point is outside the active CDT domain");
}

dt_surface_analysis CdtContext::analyze_surface_xy(
    const dt_point3& query_point) const {
    if (!finite_value(query_point.x) || !finite_value(query_point.y)) {
        throw Exception(DT_E_INVALID_ARGUMENT,
                        "CDT surface analysis XY must be finite");
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (state_->triangulation.dimension() != 2) {
        throw Exception(DT_E_EMPTY, "CDT has no two-dimensional surface");
    }
    Cdt::Locate_type type;
    int index = 0;
    const auto face = state_->triangulation.locate(
        Kernel::Point_2(query_point.x, query_point.y), type, index);
    CdtFaceHandle support;
    uint32_t flags = 0;
    if (type == Cdt::FACE) {
        if (domain_face(*state_, face)) support = face;
    } else if (type == Cdt::EDGE) {
        flags |= DT_SURFACE_QUERY_ON_EDGE;
        if (domain_face(*state_, face)) support = face;
        else if (domain_face(*state_, face->neighbor(index)))
            support = face->neighbor(index);
    } else if (type == Cdt::VERTEX) {
        flags |= DT_SURFACE_QUERY_ON_VERTEX;
        const auto vertex = face->vertex(index);
        auto incident = state_->triangulation.incident_faces(vertex);
        if (incident != 0) {
            const auto begin = incident;
            do {
                if (domain_face(*state_, incident)) {
                    support = incident;
                    break;
                }
                ++incident;
            } while (incident != begin);
        }
    }
    if (support == CdtFaceHandle()) {
        throw Exception(DT_E_NOT_FOUND,
                        "query point is outside the active CDT domain");
    }
    return analyze_triangle_surface(to_triangle(support), query_point, flags);
}

void CdtContext::visit_domain_triangles(
    const std::function<void(const dt_triangle3&)>& visitor) const {
    if (!visitor) {
        throw Exception(DT_E_INVALID_ARGUMENT, "triangle visitor is empty");
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (auto face = state_->triangulation.finite_faces_begin();
         face != state_->triangulation.finite_faces_end(); ++face) {
        if (domain_face(*state_, face)) visitor(to_triangle(face));
    }
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
                           state_->generation + 1, crs, crossing_policy_,
                           crossing_z_tolerance_);
    const auto bounds = next->bounds;
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    return bounds;
}

void CdtContext::save_binary(const char* utf8_file_name) const {
    require_file_name(utf8_file_name);
    const uint32_t endian = kCdtBinaryEndian;
    if (*reinterpret_cast<const unsigned char*>(&endian) != 0x04 ||
        sizeof(dt_point3) != sizeof(double) * 3 ||
        !std::numeric_limits<double>::is_iec559) {
        throw Exception(DT_E_UNSUPPORTED,
                        "DCDTB requires little-endian IEEE-754 doubles");
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (state_->crs_wkt.size() > kCdtBinaryHeaderSize - kCdtBinaryCrsOffset)
        throw Exception(DT_E_LIMIT_EXCEEDED, "DCDTB CRS is too large");

    uint64_t total_constraint_points = 0;
    for (const auto& constraint : state_->constraints) {
        total_constraint_points = checked_add(
            total_constraint_points, constraint.points.size(),
            "DCDTB constraint point count is too large");
    }
    const uint64_t base_bytes = checked_mul(state_->base_points.size(),
                                            sizeof(dt_point3),
                                            "DCDTB base points are too large");
    const uint64_t directory_bytes = checked_mul(
        state_->constraints.size(), kCdtBinaryDirectoryRecordSize,
        "DCDTB directory is too large");
    const uint64_t constraint_bytes = checked_mul(
        total_constraint_points, sizeof(dt_point3),
        "DCDTB constraint points are too large");
    const uint64_t base_offset = kCdtBinaryHeaderSize;
    const uint64_t directory_offset = checked_add(
        base_offset, base_bytes, "DCDTB file is too large");
    const uint64_t constraint_points_offset = checked_add(
        directory_offset, directory_bytes, "DCDTB file is too large");
    const uint64_t file_size = checked_add(
        constraint_points_offset, constraint_bytes, "DCDTB file is too large");

    std::ofstream output(std::filesystem::u8path(utf8_file_name),
                         std::ios::binary | std::ios::trunc);
    if (!output) throw Exception(DT_E_IO, "cannot create DCDTB file");
    std::array<unsigned char, kCdtBinaryHeaderSize> header{};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    uint64_t hash = 1469598103934665603ULL;
    auto write_payload = [&](const void* data, size_t size) {
        output.write(static_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
        if (!output) throw Exception(DT_E_IO, "failed to write DCDTB payload");
        hash = fnv_update(hash, data, size);
    };
    if (!state_->base_points.empty()) {
        write_payload(state_->base_points.data(),
                      state_->base_points.size() * sizeof(dt_point3));
    }
    uint64_t point_offset = constraint_points_offset;
    for (const auto& constraint : state_->constraints) {
        std::array<unsigned char, kCdtBinaryDirectoryRecordSize> record{};
        std::memcpy(record.data(), &constraint.id, sizeof(constraint.id));
        std::memcpy(record.data() + 8, &constraint.kind,
                    sizeof(constraint.kind));
        std::memcpy(record.data() + 12, &constraint.flags,
                    sizeof(constraint.flags));
        const uint64_t count = constraint.points.size();
        std::memcpy(record.data() + 16, &count, sizeof(count));
        std::memcpy(record.data() + 24, &point_offset, sizeof(point_offset));
        dt_bounds2 bounds{constraint.points.front().x,
                          constraint.points.front().y,
                          constraint.points.front().x,
                          constraint.points.front().y};
        for (const auto& point : constraint.points) {
            bounds.xmin = std::min(bounds.xmin, point.x);
            bounds.ymin = std::min(bounds.ymin, point.y);
            bounds.xmax = std::max(bounds.xmax, point.x);
            bounds.ymax = std::max(bounds.ymax, point.y);
        }
        std::memcpy(record.data() + 32, &bounds, sizeof(bounds));
        write_payload(record.data(), record.size());
        point_offset = checked_add(
            point_offset, checked_mul(count, sizeof(dt_point3),
                                      "DCDTB point offset overflow"),
            "DCDTB point offset overflow");
    }
    for (const auto& constraint : state_->constraints) {
        write_payload(constraint.points.data(),
                      constraint.points.size() * sizeof(dt_point3));
    }
    const auto written_size = output.tellp();
    if (written_size < 0 || static_cast<uint64_t>(written_size) != file_size)
        throw Exception(DT_E_IO, "DCDTB output size mismatch");

    std::memcpy(header.data(), kCdtBinaryMagic, sizeof(kCdtBinaryMagic));
    binary_store(header, 8, kCdtBinaryVersion);
    binary_store(header, 12, kCdtBinaryEndian);
    binary_store(header, 16, static_cast<uint64_t>(kCdtBinaryHeaderSize));
    binary_store(header, 24, file_size);
    binary_store(header, 32, static_cast<uint64_t>(state_->base_points.size()));
    binary_store(header, 40, static_cast<uint64_t>(state_->constraints.size()));
    binary_store(header, 48, state_->next_constraint_id);
    binary_store(header, 56, base_offset);
    binary_store(header, 64, directory_offset);
    binary_store(header, 72, constraint_points_offset);
    binary_store(header, 80, static_cast<uint64_t>(state_->crs_wkt.size()));
    binary_store(header, 88, hash);
    binary_store(header, 96, total_constraint_points);
    binary_store(header, 104, crossing_policy_);
    binary_store(header, 112, crossing_z_tolerance_);
    std::memcpy(header.data() + kCdtBinaryCrsOffset, state_->crs_wkt.data(),
                state_->crs_wkt.size());
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    output.flush();
    if (!output) throw Exception(DT_E_IO, "failed to finalize DCDTB file");
}

void CdtContext::verify_binary_file(const char* utf8_file_name) {
    require_file_name(utf8_file_name);
    std::ifstream input(std::filesystem::u8path(utf8_file_name),
                        std::ios::binary);
    if (!input) throw Exception(DT_E_IO, "cannot open DCDTB file");
    const auto metadata = read_binary_metadata(input);
    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata.base_offset));
    uint64_t hash = 1469598103934665603ULL;
    std::array<unsigned char, 1024 * 1024> buffer{};
    uint64_t remaining = metadata.file_size - metadata.base_offset;
    while (remaining != 0) {
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(count));
        if (!input) throw Exception(DT_E_CORRUPTED_DATA,
                                    "truncated DCDTB payload");
        hash = fnv_update(hash, buffer.data(), count);
        remaining -= count;
    }
    if (hash != metadata.payload_hash)
        throw Exception(DT_E_CORRUPTED_DATA, "DCDTB payload checksum mismatch");
}

std::vector<dt_cdt_binary_index_entry> CdtContext::query_binary_index(
    const char* utf8_file_name, const dt_bounds2& bounds) {
    require_file_name(utf8_file_name);
    validate_bounds(bounds);
    std::ifstream input(std::filesystem::u8path(utf8_file_name),
                        std::ios::binary);
    if (!input) throw Exception(DT_E_IO, "cannot open DCDTB file");
    const auto metadata = read_binary_metadata(input);
    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata.directory_offset));
    std::vector<dt_cdt_binary_index_entry> result;
    for (uint64_t i = 0; i < metadata.constraint_count; ++i) {
        const auto entry = read_binary_directory_entry(input);
        const uint64_t bytes = checked_mul(entry.point_count, sizeof(dt_point3),
                                           "DCDTB entry is too large");
        if (entry.point_offset < metadata.constraint_points_offset ||
            entry.point_offset > metadata.file_size ||
            bytes > metadata.file_size - entry.point_offset)
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DCDTB entry point offset is invalid");
        if (bounds_intersect(entry.bounds, bounds)) result.push_back(entry);
    }
    return result;
}

CdtConstraint CdtContext::read_binary_constraint(
    const char* utf8_file_name, dt_constraint_id id) {
    if (id == 0) throw Exception(DT_E_INVALID_ARGUMENT,
                                 "constraint id is zero");
    require_file_name(utf8_file_name);
    std::ifstream input(std::filesystem::u8path(utf8_file_name),
                        std::ios::binary);
    if (!input) throw Exception(DT_E_IO, "cannot open DCDTB file");
    const auto metadata = read_binary_metadata(input);
    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata.directory_offset));
    dt_cdt_binary_index_entry selected{};
    bool found = false;
    for (uint64_t i = 0; i < metadata.constraint_count; ++i) {
        const auto entry = read_binary_directory_entry(input);
        if (entry.id == id) {
            selected = entry;
            found = true;
            break;
        }
    }
    if (!found) throw Exception(DT_E_NOT_FOUND,
                                "binary constraint id was not found");
    const uint64_t bytes = checked_mul(selected.point_count, sizeof(dt_point3),
                                       "DCDTB entry is too large");
    if (selected.point_offset < metadata.constraint_points_offset ||
        selected.point_offset > metadata.file_size ||
        bytes > metadata.file_size - selected.point_offset)
        throw Exception(DT_E_CORRUPTED_DATA,
                        "DCDTB entry point offset is invalid");
    CdtConstraint result;
    result.id = selected.id;
    result.kind = selected.kind;
    result.flags = selected.flags;
    result.points.resize(static_cast<size_t>(selected.point_count));
    input.clear();
    input.seekg(static_cast<std::streamoff>(selected.point_offset));
    input.read(reinterpret_cast<char*>(result.points.data()),
               static_cast<std::streamsize>(bytes));
    if (!input) throw Exception(DT_E_CORRUPTED_DATA,
                                "truncated DCDTB constraint points");
    for (const auto& point : result.points) validate_point(point);
    return result;
}

dt_bounds2 CdtContext::load_binary(const char* utf8_file_name) {
    verify_binary_file(utf8_file_name);
    std::ifstream input(std::filesystem::u8path(utf8_file_name),
                        std::ios::binary);
    if (!input) throw Exception(DT_E_IO, "cannot open DCDTB file");
    const auto metadata = read_binary_metadata(input);
    std::vector<dt_point3> points(static_cast<size_t>(metadata.base_count));
    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata.base_offset));
    input.read(reinterpret_cast<char*>(points.data()),
               static_cast<std::streamsize>(points.size() * sizeof(dt_point3)));
    if (!input && !points.empty())
        throw Exception(DT_E_CORRUPTED_DATA, "truncated DCDTB base points");
    for (const auto& point : points) validate_point(point);

    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata.directory_offset));
    std::vector<dt_cdt_binary_index_entry> entries;
    entries.reserve(static_cast<size_t>(metadata.constraint_count));
    for (uint64_t i = 0; i < metadata.constraint_count; ++i)
        entries.push_back(read_binary_directory_entry(input));
    std::vector<CdtConstraint> constraints;
    constraints.reserve(entries.size());
    uint64_t expected_offset = metadata.constraint_points_offset;
    dt_constraint_id maximum_id = 0;
    for (const auto& entry : entries) {
        const uint64_t bytes = checked_mul(entry.point_count, sizeof(dt_point3),
                                           "DCDTB entry is too large");
        if (entry.point_offset != expected_offset ||
            bytes > metadata.file_size - entry.point_offset)
            throw Exception(DT_E_CORRUPTED_DATA,
                            "DCDTB directory is not sequential");
        CdtConstraint constraint;
        constraint.id = entry.id;
        constraint.kind = entry.kind;
        constraint.flags = entry.flags;
        maximum_id = std::max(maximum_id, entry.id);
        constraint.points.resize(static_cast<size_t>(entry.point_count));
        input.clear();
        input.seekg(static_cast<std::streamoff>(entry.point_offset));
        input.read(reinterpret_cast<char*>(constraint.points.data()),
                   static_cast<std::streamsize>(bytes));
        if (!input) throw Exception(DT_E_CORRUPTED_DATA,
                                    "truncated DCDTB constraint points");
        for (const auto& point : constraint.points) validate_point(point);
        constraints.push_back(std::move(constraint));
        expected_offset = checked_add(expected_offset, bytes,
                                      "DCDTB point offset overflow");
    }
    if (metadata.next_constraint_id == 0 ||
        metadata.next_constraint_id <= maximum_id) {
        throw Exception(DT_E_CORRUPTED_DATA,
                        "DCDTB next constraint id is invalid");
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto next = make_state(points, constraints, metadata.next_constraint_id,
                           state_->generation + 1, metadata.crs,
                           crossing_policy_, crossing_z_tolerance_);
    const auto bounds = next->bounds;
    state_.swap(next);
    ++full_rebuild_count_;
    last_edit_mode_ = DT_CDT_EDIT_MODE_FULL_REBUILD;
    return bounds;
}

} // namespace dt
