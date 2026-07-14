#ifndef DT_LEGACY_HPP
#define DT_LEGACY_HPP

#include "dt_api.h"

/*
 * Compatibility interface for the original MSVC/MinGW C++ API.
 * Returned double arrays are owned by the DLL and remain valid only until the
 * next legacy call on the same thread. Callers must not free them.
 */
extern "C" {

DT_API void DT_CALL dt_init_dll();
DT_API void DT_CALL dt_free_dll();

DT_API bool DT_CALL dt_insert_a_point_with_draw(
    const double& x, const double& y, const double& z,
    int& f, int& h, int& e, double*& pEffect);
DT_API bool DT_CALL dt_insert_a_point(
    const double& x, const double& y, const double& z);

DT_API bool DT_CALL dt_delete_a_point_with_draw(
    int& f, int& h, int& e, double*& pEffect,
    const double& x, const double& y, const double& z);
DT_API bool DT_CALL dt_delete_a_point(
    const double& x, const double& y, const double& z);

DT_API void DT_CALL dt_clear();
DT_API void DT_CALL dt_save_triangulation(const char* fileName);
DT_API void DT_CALL dt_load_triangulation(
    const char* fileName, double& xmin, double& ymin, double& xmax, double& ymax);

DT_API void DT_CALL dt_view_to_range(
    double*& pShowTri, int& TriNum,
    const double& xmin, const double& ymin,
    const double& xmax, const double& ymax);

DT_API bool DT_CALL dt_get_a_point_nearest_point(
    double& gx, double& gy, double& gz,
    const double& x, const double& y, const double& z);

DT_API bool DT_CALL dt_get_a_triangle_covers_point(
    double& gx0, double& gy0, double& gz0,
    double& gx1, double& gy1, double& gz1,
    double& gx2, double& gy2, double& gz2,
    const double& x, const double& y, const double& z);

}

#endif

