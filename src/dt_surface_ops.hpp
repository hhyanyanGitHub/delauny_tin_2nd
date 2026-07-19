#ifndef DT_SURFACE_OPS_HPP
#define DT_SURFACE_OPS_HPP

#include "dt_cdt_core.hpp"
#include "dt_terrain_core.hpp"

#include <memory>
#include <vector>

namespace dt {

struct SurfaceClipData {
    std::vector<dt_point3> points;
    std::vector<dt_surface_clip_ring> rings;
    std::vector<dt_surface_clip_piece> pieces;
    uint64_t source_triangle_count = 0;
    uint64_t generation = 0;
    double exact_plan_area = 0.0;
};

std::unique_ptr<SurfaceClipData> clip_tin_polygon_exact(
    Context& tin, const dt_polygon_rings& polygon);
std::unique_ptr<SurfaceClipData> clip_cdt_polygon_exact(
    CdtContext& cdt, const dt_polygon_rings& polygon);

dt_surface_registration_result register_grid_surfaces(
    const Grid& reference, const Grid& moving,
    const dt_surface_registration_options& options);
dt_surface_error_result compare_grid_surfaces_adaptive(
    const Grid& reference, const Grid& moving,
    const dt_surface_error_options& options);
std::unique_ptr<Grid> apply_grid_registration(
    const Grid& moving, const Grid& reference,
    const dt_surface_registration_result& registration);

} // namespace dt

#endif
