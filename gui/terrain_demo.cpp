#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include "dt_api.h"
#include "dt_cdt_api.h"
#include "dt_gdal_api.h"
#include "dt_task_api.h"
#include "dt_terrain_api.h"
#include "terrain_3d.hpp"
#include "terrain_measurement.hpp"
#include "terrain_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kToolbarHeight = 48;
constexpr int kStatusHeight = 28;
constexpr size_t kMaxDrawTriangles = 45000;
constexpr size_t kMaxDrawTriangles3d = 18000;
constexpr uint64_t kMaxContourDrawVertices = 200000;
constexpr UINT_PTR kGridPreviewTimer = 1;
constexpr UINT kGridPreviewPollMilliseconds = 25;

enum CommandId {
    ID_GENERATE_100K = 100,
    ID_GENERATE_1M,
    ID_CLEAR,
    ID_MODE_INSERT,
    ID_MODE_DELETE,
    ID_MODE_QUERY,
    ID_MODE_ZOOM_BOX,
    ID_FULL_EXTENT,
    ID_IMPORT_POINTS,
    ID_SAVE,
    ID_LOAD,
    ID_VIEW_3D,
    ID_Z_EXAGGERATION,
    ID_TIN_TO_GRID = 200,
    ID_GRID_TO_TIN,
    ID_CONTOURS_FROM_TIN,
    ID_CONTOURS_FROM_GRID,
    ID_TIN_FROM_CONTOURS,
    ID_GRID_FROM_CONTOURS,
    ID_CDT_FROM_TIN,
    ID_GRID_FROM_CDT,
    ID_CONTOURS_FROM_CDT,
    ID_IMPORT_GRID,
    ID_EXPORT_GRID,
    ID_VERIFY_DGRIDB,
    ID_COMPACT_DGTILE,
    ID_IMPORT_CONTOURS,
    ID_EXPORT_CONTOURS,
    ID_IMPORT_GDAL_RASTER,
    ID_EXPORT_GEOTIFF,
    ID_EXPORT_COG,
    ID_IMPORT_GDAL_CONTOURS,
    ID_EXPORT_GPKG_CONTOURS,
    ID_IMPORT_CDT,
    ID_EXPORT_CDT,
    ID_LAYER_TIN,
    ID_LAYER_GRID,
    ID_LAYER_CONTOURS,
    ID_LAYER_CDT,
    ID_CDT_DRAW_BREAKLINE,
    ID_CDT_DRAW_OUTER,
    ID_CDT_DRAW_HOLE,
    ID_CDT_BATCH_SAMPLE,
    ID_CDT_FINISH,
    ID_CDT_CANCEL,
    ID_CDT_MOVE_VERTEX,
    ID_CDT_REMOVE_VERTEX,
    ID_CDT_DELETE,
    ID_PROFILE_MODE,
    ID_PROFILE_EXPORT,
    ID_PROFILE_CLEAR,
    ID_MEASURE_MODE,
    ID_MEASURE_FINISH,
    ID_MEASURE_DATUM,
    ID_MEASURE_EXPORT,
    ID_MEASURE_CLEAR,
    ID_GRID_MASK_MEASUREMENT,
    ID_GRID_CLIP_MEASUREMENT,
    ID_GRID_INVERT_MEASUREMENT,
    ID_SLOPE_MODE,
    ID_SLOPE_CLEAR,
    ID_TERRAIN_SLOPE_GRID,
    ID_TERRAIN_ASPECT_GRID,
    ID_TERRAIN_HILLSHADE_GRID,
    ID_TERRAIN_PARAMETERS,
    ID_TERRAIN_CANCEL,
    ID_TERRAIN_SHOW_ELEVATION,
    ID_TERRAIN_EXPORT_GRID,
    ID_TERRAIN_EXPORT_GEOTIFF,
    ID_EARTHWORK_LOAD_DESIGN,
    ID_EARTHWORK_LOAD_DESIGN_GDAL,
    ID_EARTHWORK_RESAMPLE_BILINEAR,
    ID_EARTHWORK_RESAMPLE_NEAREST,
    ID_EARTHWORK_OFFSET_DESIGN,
    ID_EARTHWORK_RUN,
    ID_EARTHWORK_EXPORT,
    ID_EARTHWORK_CLEAR
};

enum class Mode {
    Insert,
    Delete,
    Query,
    ZoomBox,
    CdtBreakline,
    CdtOuter,
    CdtHole,
    CdtMoveVertex,
    CdtRemoveVertex,
    CdtDelete,
    Profile,
    Measure,
    Slope
};
enum class ViewMode { Map2D, Terrain3D };
enum class Drag3D { None, Orbit, Pan };
enum class ProfileSource { None, Cdt, Tin, Grid };
enum class GridTheme { Elevation, Slope, Aspect, Hillshade, Difference };

struct DoublePromptState {
    HWND edit = nullptr;
    bool done = false;
    bool accepted = false;
    double value = 0.0;
    const wchar_t* label = nullptr;
};

LRESULT CALLBACK double_prompt_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DoublePromptState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<DoublePromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE: {
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND label = CreateWindowExW(0, L"STATIC", state->label,
            WS_CHILD | WS_VISIBLE, 18, 18, 310, 22, window, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            18, 46, 310, 25, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
            GetModuleHandleW(nullptr), nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            166, 88, 76, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
            GetModuleHandleW(nullptr), nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            252, 88, 76, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        std::wostringstream text;
        text << std::setprecision(15) << state->value;
        SetWindowTextW(state->edit, text.str().c_str());
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wparam) == IDOK) {
            wchar_t buffer[128]{};
            GetWindowTextW(state->edit, buffer, 128);
            wchar_t* end = nullptr;
            const double parsed = std::wcstod(buffer, &end);
            while (end && std::iswspace(*end)) ++end;
            if (end == buffer || (end && *end != L'\0') ||
                !std::isfinite(parsed)) {
                MessageBoxW(window, L"请输入有限数值，例如 100.5。",
                            L"无效基准高程", MB_OK | MB_ICONWARNING);
                SetFocus(state->edit);
                SendMessageW(state->edit, EM_SETSEL, 0, -1);
                return 0;
            }
            state->value = parsed;
            state->accepted = true;
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) state->done = true;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool prompt_double(HWND owner, const wchar_t* title, const wchar_t* label,
                   double& value) {
    static const wchar_t* class_name = L"dterrain_double_prompt";
    static ATOM prompt_class = 0;
    if (!prompt_class) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = double_prompt_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        prompt_class = RegisterClassW(&window_class);
        if (!prompt_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }
    DoublePromptState state{};
    state.value = value;
    state.label = label;
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    constexpr int width = 364;
    constexpr int height = 165;
    const int x = (owner_rect.left + owner_rect.right - width) / 2;
    const int y = (owner_rect.top + owner_rect.bottom - height) / 2;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, class_name, title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP, x, y, width, height,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    bool saw_quit = false;
    while (!state.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            saw_quit = result == 0;
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (saw_quit) PostQuitMessage(static_cast<int>(message.wParam));
    if (state.accepted) value = state.value;
    return state.accepted;
}

std::wstring utf8_to_wide(const char* text) {
    if (!text || !*text) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    result.pop_back();
    return result;
}

std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count,
                        nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring last_error_text() {
    char buffer[1024]{};
    dt_get_last_error(buffer, sizeof(buffer), nullptr);
    return utf8_to_wide(buffer);
}

double terrain_z(double x, double y) {
    const double rolling = 75.0 * std::sin(x * 0.0031) * std::cos(y * 0.0023);
    const double ridge = 42.0 * std::sin((x + y) * 0.0065);
    const double hill1 = 130.0 * std::exp(-((x - 2500.0) * (x - 2500.0) +
                                            (y - 3500.0) * (y - 3500.0)) /
                                          1800000.0);
    const double hill2 = 95.0 * std::exp(-((x - 7200.0) * (x - 7200.0) +
                                           (y - 6200.0) * (y - 6200.0)) /
                                         1100000.0);
    return rolling + ridge + hill1 + hill2;
}

double jitter(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<double>(value & 0xffffU) / 65535.0 - 0.5;
}

class DemoApp {
public:
    DemoApp() {
        dt_create(nullptr, &mesh_);
        dt_cdt_create(nullptr, &cdt_);
        if (dt_gdal_initialize() == DT_OK) {
            int32_t available = 0;
            if (dt_gdal_is_driver_available("GTiff", &available) == DT_OK)
                gdal_gtiff_available_ = available != 0;
            available = 0;
            if (dt_gdal_is_driver_available("COG", &available) == DT_OK)
                gdal_cog_available_ = available != 0;
            available = 0;
            if (dt_gdal_is_driver_available("GPKG", &available) == DT_OK)
                gdal_gpkg_available_ = available != 0;
        }
    }
    ~DemoApp() {
        shutdown_grid_preview_tasks();
        destroy_grid_view_cache();
        dt_cdt_destroy(cdt_);
        dt_contours_destroy(contours_);
        dt_grid_destroy(terrain_grid_);
        dt_grid_destroy(design_grid_);
        dt_grid_destroy(grid_);
        dt_destroy(mesh_);
    }

    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        hwnd_ = window;
        switch (message) {
        case WM_CREATE:
            create_controls();
            create_menus();
            generate(100000);
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wparam));
            return 0;
        case WM_CLOSE:
            if (terrain_task_running_) {
                close_after_terrain_task_ = true;
                dt_task_request_cancel(terrain_task_);
                action_text_ = L"正在取消专题计算，完成后关闭窗口…";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_SIZE:
            layout_controls();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_TIMER:
            if (wparam == kGridPreviewTimer) {
                poll_grid_preview_tasks();
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        case WM_LBUTTONDOWN:
            if (GET_Y_LPARAM(lparam) >= kToolbarHeight) {
                SetFocus(hwnd_);
                if (view_mode_ == ViewMode::Terrain3D)
                    begin_drag3d(Drag3D::Orbit, GET_X_LPARAM(lparam),
                                 GET_Y_LPARAM(lparam));
                else if (mode_ == Mode::ZoomBox)
                    begin_box_zoom(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                else
                    on_left_click(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag3d_ != Drag3D::None)
                end_drag3d();
            else if (box_zooming_)
                end_box_zoom(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            SetFocus(hwnd_);
            if (view_mode_ == ViewMode::Terrain3D)
                begin_drag3d(Drag3D::Pan, GET_X_LPARAM(lparam),
                             GET_Y_LPARAM(lparam));
            else
                begin_pan(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_MOUSEMOVE:
            if (drag3d_ != Drag3D::None)
                drag3d_to(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            else if (panning_) pan_to(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            else if (box_zooming_)
                update_box_zoom(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (drag3d_ != Drag3D::None) end_drag3d();
            else end_pan();
            if (GetCapture() == hwnd_) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &point);
            if (view_mode_ == ViewMode::Terrain3D)
                zoom3d(GET_WHEEL_DELTA_WPARAM(wparam));
            else
                zoom(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }
        case WM_KEYDOWN:
            if (terrain_task_running_) {
                if (wparam == VK_ESCAPE) {
                    dt_task_request_cancel(terrain_task_);
                    action_text_ = L"已请求取消专题计算…";
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            if (view_mode_ == ViewMode::Terrain3D && handle_key3d(wparam)) return 0;
            if (wparam == VK_RETURN && mode_ == Mode::Measure) {
                finish_measurement();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_BACK && mode_ == Mode::Measure &&
                !measurement_complete_ && !measurement_polygon_.empty()) {
                measurement_polygon_.pop_back();
                action_text_ = L"已撤销量测多边形最后一点";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && mode_ == Mode::Measure &&
                !measurement_polygon_.empty()) {
                clear_measurement();
                action_text_ = L"已清除当前面积/土方量测";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_RETURN && is_cdt_draw_mode()) {
                finish_cdt_draft();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_BACK && is_cdt_draw_mode() && !cdt_draft_.empty()) {
                cdt_draft_.pop_back();
                action_text_ = L"已撤销约束草图最后一点";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && !cdt_draft_.empty()) {
                cancel_cdt_draft();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && mode_ == Mode::CdtMoveVertex &&
                cdt_move_constraint_id_ != 0) {
                reset_cdt_move_selection();
                action_text_ = L"已取消约束顶点选择";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && box_zooming_) {
                cancel_box_zoom();
                action_text_ = L"已取消框选放大";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && mode_ == Mode::Slope && slope_valid_) {
                clear_slope_analysis();
                action_text_ = L"已清除当前坡度/坡向分析结果";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE && mode_ == Mode::Profile &&
                profile_has_start_) {
                clear_profile();
                action_text_ = L"已清除当前剖面";
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

private:
    HWND hwnd_ = nullptr;
    dt_handle mesh_ = nullptr;
    dt_cdt_handle cdt_ = nullptr;
    dt_grid_handle grid_ = nullptr;
    dt_grid_handle terrain_grid_ = nullptr;
    dt_grid_handle design_grid_ = nullptr;
    dt_contour_handle contours_ = nullptr;
    Mode mode_ = Mode::Query;
    ViewMode view_mode_ = ViewMode::Map2D;
    dt_bounds2 view_{0.0, 0.0, 1.0, 1.0};
    bool view_valid_ = false;
    bool cache_valid_ = false;
    std::vector<dt_triangle3> triangles_;
    std::vector<dt_triangle3> cdt_triangles_;
    struct ConstraintLine {
        dt_constraint_id id = 0;
        int32_t kind = DT_CONSTRAINT_BREAKLINE;
        uint32_t flags = 0;
        std::vector<dt_point3> points;
    };
    std::vector<ConstraintLine> constraint_lines_;
    std::vector<dt_point3> cdt_draft_;
    int32_t cdt_draft_kind_ = DT_CONSTRAINT_BREAKLINE;
    dt_constraint_id cdt_move_constraint_id_ = 0;
    size_t cdt_move_point_index_ = std::numeric_limits<size_t>::max();
    dt_cdt_statistics cdt_info_{};
    std::vector<dt_triangle3> removed_triangles_;
    std::vector<dt_triangle3> added_triangles_;
    std::vector<dt_segment3> boundary_edges_;
    std::vector<dt_segment3> added_edges_;
    bool has_highlight_vertex_ = false;
    bool has_highlight_triangle_ = false;
    dt_vertex3 highlight_vertex_{};
    dt_triangle3 highlight_triangle_{};
    std::wstring action_text_ = L"准备就绪";
    double last_query_ms_ = 0.0;
    bool panning_ = false;
    POINT pan_start_{};
    dt_bounds2 pan_view_{};
    bool box_zooming_ = false;
    POINT box_start_{};
    POINT box_end_{};
    std::vector<HWND> controls_;
    HMENU layer_menu_ = nullptr;
    bool show_tin_ = true;
    bool show_grid_ = true;
    bool show_contours_ = true;
    bool show_cdt_ = true;
    bool gdal_gtiff_available_ = false;
    bool gdal_cog_available_ = false;
    bool gdal_gpkg_available_ = false;
    dt_grid_info grid_info_{};
    std::vector<uint32_t> grid_preview_;
    uint32_t grid_preview_width_ = 0;
    uint32_t grid_preview_height_ = 0;
    uint64_t grid_preview_column_ = 0;
    uint64_t grid_preview_row_ = 0;
    uint64_t grid_preview_source_width_ = 0;
    uint64_t grid_preview_source_height_ = 0;
    dt_grid_handle grid_preview_source_ = nullptr;
    bool grid_preview_view_valid_ = false;
    bool grid_preview_used_pyramid_ = false;
    bool grid_preview_used_tile_cache_ = false;
    bool grid_preview_used_disk_cache_ = false;
    uint64_t grid_preview_lod_scale_ = 1;
    uint64_t grid_preview_tile_count_ = 0;
    uint64_t grid_preview_reused_tile_count_ = 0;
    GridTheme grid_preview_theme_ = GridTheme::Elevation;
    dt_grid_view_cache_handle grid_view_cache_ = nullptr;
    dt_grid_handle grid_view_cache_source_ = nullptr;
    std::string grid_disk_cache_file_;
    uint64_t grid_disk_cache_revision_ = 0;
    struct GridPreviewRequest {
        dt_task_handle task = nullptr;
        dt_grid_handle source = nullptr;
        dt_grid_info source_info{};
        dt_grid_window window{};
        uint64_t output_width = 0;
        uint64_t output_height = 0;
        uint64_t last_frame_sequence = 0;
        bool final_frame_consumed = false;
        GridTheme theme = GridTheme::Elevation;
    } grid_preview_request_;
    GridPreviewRequest failed_grid_preview_request_;
    std::vector<dt_task_handle> retired_grid_preview_tasks_;
    double grid_zmin_ = 0.0;
    double grid_zmax_ = 1.0;
    GridTheme grid_theme_ = GridTheme::Elevation;
    dt_task_handle terrain_task_ = nullptr;
    bool terrain_task_running_ = false;
    bool close_after_terrain_task_ = false;
    double terrain_z_factor_ = 1.0;
    double terrain_sun_azimuth_ = 315.0;
    double terrain_sun_altitude_ = 45.0;
    uint32_t terrain_worker_count_ = 0;
    uint32_t terrain_tile_rows_ = 0;
    dt_grid_earthwork_result earthwork_result_{};
    bool earthwork_result_valid_ = false;
    dt_contour_info contour_info_{};
    HWND view_button_ = nullptr;
    HWND exaggeration_button_ = nullptr;
    dterrain::viewer3d::Camera camera_{};
    Drag3D drag3d_ = Drag3D::None;
    POINT drag3d_last_{};
    bool camera_needs_reset_ = true;
    double model_center_x_ = 0.0;
    double model_center_y_ = 0.0;
    double model_center_z_ = 0.0;
    double model_xy_scale_ = 1.0;
    double z_exaggeration_ = 1.0;
    double mesh_zmin_ = 0.0;
    double mesh_zmax_ = 1.0;
    ProfileSource profile_source_ = ProfileSource::None;
    bool profile_has_start_ = false;
    bool profile_complete_ = false;
    dt_point3 profile_start_{};
    dt_point3 profile_end_{};
    std::vector<dterrain::profile::Sample> profile_samples_;
    dterrain::profile::Statistics profile_statistics_{};
    ProfileSource measurement_source_ = ProfileSource::None;
    bool measurement_complete_ = false;
    double measurement_datum_z_ = 0.0;
    std::vector<dt_point3> measurement_polygon_;
    dterrain::measurement::Statistics measurement_statistics_{};
    ProfileSource slope_source_ = ProfileSource::None;
    bool slope_valid_ = false;
    dt_surface_analysis slope_analysis_{};

    RECT canvas_rect() const {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        rect.top = kToolbarHeight;
        rect.bottom = std::max(rect.top + 1, rect.bottom - kStatusHeight);
        return rect;
    }

    static HWND make_button(HWND parent, int id, const wchar_t* text) {
        return CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 80, 30, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    void create_controls() {
        controls_.push_back(make_button(hwnd_, ID_GENERATE_100K, L"模拟10万"));
        controls_.push_back(make_button(hwnd_, ID_GENERATE_1M, L"模拟100万"));
        controls_.push_back(make_button(hwnd_, ID_CLEAR, L"清空"));
        controls_.push_back(make_button(hwnd_, ID_MODE_QUERY, L"查询模式"));
        controls_.push_back(make_button(hwnd_, ID_MODE_INSERT, L"插入模式"));
        controls_.push_back(make_button(hwnd_, ID_MODE_DELETE, L"删除模式"));
        controls_.push_back(make_button(hwnd_, ID_MODE_ZOOM_BOX, L"框选放大"));
        controls_.push_back(make_button(hwnd_, ID_FULL_EXTENT, L"全图"));
        controls_.push_back(make_button(hwnd_, ID_IMPORT_POINTS, L"导入XYZ"));
        controls_.push_back(make_button(hwnd_, ID_SAVE, L"保存网格"));
        controls_.push_back(make_button(hwnd_, ID_LOAD, L"打开网格"));
        view_button_ = make_button(hwnd_, ID_VIEW_3D, L"切换3D");
        controls_.push_back(view_button_);
        exaggeration_button_ = make_button(hwnd_, ID_Z_EXAGGERATION, L"高程×1.0");
        controls_.push_back(exaggeration_button_);
        EnableWindow(exaggeration_button_, FALSE);
        layout_controls();
    }

    void create_menus() {
        HMENU menu_bar = CreateMenu();
        HMENU terrain_menu = CreatePopupMenu();
        AppendMenuW(terrain_menu, MF_STRING, ID_TIN_TO_GRID,
                    L"TIN → GRID（自动 401）");
        AppendMenuW(terrain_menu, MF_STRING, ID_GRID_TO_TIN,
                    L"GRID → TIN（允许跨 NoData）");
        AppendMenuW(terrain_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(terrain_menu, MF_STRING, ID_CONTOURS_FROM_TIN,
                    L"从 TIN 生成等高线（自动间隔）");
        AppendMenuW(terrain_menu, MF_STRING, ID_CONTOURS_FROM_GRID,
                    L"从 GRID 生成等高线（自动间隔）");
        AppendMenuW(terrain_menu, MF_STRING, ID_TIN_FROM_CONTOURS,
                    L"等高线 → TIN（按折点重建）");
        AppendMenuW(terrain_menu, MF_STRING, ID_GRID_FROM_CONTOURS,
                    L"等高线 → GRID（自动 401）");
        AppendMenuW(terrain_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(terrain_menu, MF_STRING, ID_CDT_FROM_TIN,
                    L"从当前 TIN 创建约束网");
        AppendMenuW(terrain_menu, MF_STRING, ID_GRID_FROM_CDT,
                    L"约束网 → GRID（自动 401）");
        AppendMenuW(terrain_menu, MF_STRING, ID_CONTOURS_FROM_CDT,
                    L"从约束网生成等高线（自动间隔）");

        HMENU exchange_menu = CreatePopupMenu();
        AppendMenuW(exchange_menu, MF_STRING, ID_IMPORT_GRID,
                    L"导入 GRID（DGRIDB / 文本）…");
        AppendMenuW(exchange_menu, MF_STRING, ID_EXPORT_GRID,
                    L"导出 GRID（DGRIDB / 文本）…");
        AppendMenuW(exchange_menu, MF_STRING, ID_VERIFY_DGRIDB,
                    L"验证 DGRIDB 数据块…");
        AppendMenuW(exchange_menu, MF_STRING, ID_COMPACT_DGTILE,
                    L"压缩当前 DGTILE 显示缓存…");
        AppendMenuW(exchange_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(exchange_menu, MF_STRING, ID_IMPORT_CONTOURS,
                    L"导入等高线文本…");
        AppendMenuW(exchange_menu, MF_STRING, ID_EXPORT_CONTOURS,
                    L"导出等高线文本…");
        AppendMenuW(exchange_menu, MF_SEPARATOR, 0, nullptr);
        const UINT gtiff_flags = MF_STRING |
            (gdal_gtiff_available_ ? MF_ENABLED : MF_GRAYED);
        const UINT cog_flags = MF_STRING |
            (gdal_cog_available_ ? MF_ENABLED : MF_GRAYED);
        const UINT gpkg_flags = MF_STRING |
            (gdal_gpkg_available_ ? MF_ENABLED : MF_GRAYED);
        AppendMenuW(exchange_menu, gtiff_flags, ID_IMPORT_GDAL_RASTER,
                    L"导入 GeoTIFF / COG…");
        AppendMenuW(exchange_menu, gtiff_flags, ID_EXPORT_GEOTIFF,
                    L"导出 GeoTIFF（DEFLATE）…");
        AppendMenuW(exchange_menu, cog_flags, ID_EXPORT_COG,
                    L"导出 Cloud Optimized GeoTIFF…");
        AppendMenuW(exchange_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(exchange_menu, gpkg_flags, ID_IMPORT_GDAL_CONTOURS,
                    L"导入 GeoPackage 等高线…");
        AppendMenuW(exchange_menu, gpkg_flags, ID_EXPORT_GPKG_CONTOURS,
                    L"导出 GeoPackage 等高线…");
        AppendMenuW(exchange_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(exchange_menu, MF_STRING, ID_IMPORT_CDT,
                    L"打开约束网 DCDT…");
        AppendMenuW(exchange_menu, MF_STRING, ID_EXPORT_CDT,
                    L"保存约束网 DCDT…");

        HMENU cdt_edit_menu = CreatePopupMenu();
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_DRAW_BREAKLINE,
                    L"绘制断裂线");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_DRAW_OUTER,
                    L"绘制外边界");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_DRAW_HOLE,
                    L"绘制孔洞边界");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_BATCH_SAMPLE,
                    L"批量添加 12 条示例断裂线");
        AppendMenuW(cdt_edit_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_FINISH,
                    L"完成当前约束（Enter）");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_CANCEL,
                    L"取消当前约束（Esc）");
        AppendMenuW(cdt_edit_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_MOVE_VERTEX,
                    L"移动约束顶点（两次单击）");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_REMOVE_VERTEX,
                    L"删除约束顶点（单击）");
        AppendMenuW(cdt_edit_menu, MF_STRING, ID_CDT_DELETE,
                    L"选择删除约束");

        HMENU analysis_menu = CreatePopupMenu();
        AppendMenuW(analysis_menu, MF_STRING, ID_PROFILE_MODE,
                    L"任意剖面（两次单击）");
        AppendMenuW(analysis_menu, MF_STRING, ID_PROFILE_EXPORT,
                    L"导出剖面 CSV…");
        AppendMenuW(analysis_menu, MF_STRING, ID_PROFILE_CLEAR,
                    L"清除剖面");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_MEASURE_MODE,
                    L"面积/土方量测（逐点）");
        AppendMenuW(analysis_menu, MF_STRING, ID_MEASURE_FINISH,
                    L"完成面积/土方量测（Enter）");
        AppendMenuW(analysis_menu, MF_STRING, ID_MEASURE_DATUM,
                    L"设置土方基准高程…");
        AppendMenuW(analysis_menu, MF_STRING, ID_MEASURE_EXPORT,
                    L"导出量测 CSV…");
        AppendMenuW(analysis_menu, MF_STRING, ID_MEASURE_CLEAR,
                    L"清除面积/土方量测");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_GRID_MASK_MEASUREMENT,
                    L"按量测多边形掩膜当前 GRID（保持范围）");
        AppendMenuW(analysis_menu, MF_STRING, ID_GRID_CLIP_MEASUREMENT,
                    L"按量测多边形裁剪当前 GRID（紧凑适屏）");
        AppendMenuW(analysis_menu, MF_STRING, ID_GRID_INVERT_MEASUREMENT,
                    L"按量测多边形反向掩膜当前 GRID");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_LOAD_DESIGN,
                    L"加载设计面 DGRID…");
        AppendMenuW(analysis_menu,
                    MF_STRING | (gdal_gtiff_available_ ? MF_ENABLED : MF_GRAYED),
                    ID_EARTHWORK_LOAD_DESIGN_GDAL,
                    L"加载设计面 GeoTIFF/COG…");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_RESAMPLE_BILINEAR,
                    L"双线性对齐设计面到现状 GRID");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_RESAMPLE_NEAREST,
                    L"最近邻对齐设计面到现状 GRID");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_OFFSET_DESIGN,
                    L"从现状 GRID 创建偏移设计面…");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_RUN,
                    L"计算现状面—设计面挖填方");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_EXPORT,
                    L"导出双表面土方摘要 CSV…");
        AppendMenuW(analysis_menu, MF_STRING, ID_EARTHWORK_CLEAR,
                    L"清除设计面与双表面结果");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_SLOPE_MODE,
                    L"坡度/坡向分析（单击）");
        AppendMenuW(analysis_menu, MF_STRING, ID_SLOPE_CLEAR,
                    L"清除坡度/坡向结果");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_SLOPE_GRID,
                    L"生成全幅坡度专题图");
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_ASPECT_GRID,
                    L"生成全幅坡向专题图");
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_HILLSHADE_GRID,
                    L"生成阴影地形图（当前光照参数）");
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_PARAMETERS,
                    L"设置专题分析与性能参数…");
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_CANCEL,
                    L"取消正在进行的专题计算（Esc）");
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_SHOW_ELEVATION,
                    L"恢复显示高程 GRID");
        AppendMenuW(analysis_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(analysis_menu, MF_STRING, ID_TERRAIN_EXPORT_GRID,
                    L"导出当前专题图 DGRID…");
        AppendMenuW(analysis_menu,
                    MF_STRING | (gdal_gtiff_available_ ? MF_ENABLED : MF_GRAYED),
                    ID_TERRAIN_EXPORT_GEOTIFF,
                    L"导出当前专题图 GeoTIFF…");

        layer_menu_ = CreatePopupMenu();
        AppendMenuW(layer_menu_, MF_STRING | MF_CHECKED, ID_LAYER_TIN,
                    L"TIN 三角网");
        AppendMenuW(layer_menu_, MF_STRING | MF_CHECKED, ID_LAYER_GRID,
                    L"GRID 高程着色");
        AppendMenuW(layer_menu_, MF_STRING | MF_CHECKED, ID_LAYER_CONTOURS,
                    L"等高线");
        AppendMenuW(layer_menu_, MF_STRING | MF_CHECKED, ID_LAYER_CDT,
                    L"约束 Delaunay");

        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(terrain_menu),
                    L"地形转换(&T)");
        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(exchange_menu),
                    L"数据交换(&E)");
        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(cdt_edit_menu),
                    L"约束编辑(&C)");
        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(analysis_menu),
                    L"分析(&A)");
        AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_menu_),
                    L"图层(&L)");
        SetMenu(hwnd_, menu_bar);
    }

    void update_layer_menu() const {
        if (!layer_menu_) return;
        const wchar_t* grid_label = grid_theme_ == GridTheme::Slope
                                        ? L"GRID 坡度专题"
                                        : grid_theme_ == GridTheme::Aspect
                                              ? L"GRID 坡向专题"
                                              : grid_theme_ == GridTheme::Hillshade
                                                    ? L"GRID 阴影地形"
                                                    : grid_theme_ == GridTheme::Difference
                                                          ? L"GRID 现状-设计高差"
                                                          : L"GRID 高程着色";
        ModifyMenuW(layer_menu_, ID_LAYER_GRID,
                    MF_BYCOMMAND | MF_STRING, ID_LAYER_GRID, grid_label);
        CheckMenuItem(layer_menu_, ID_LAYER_TIN,
                      MF_BYCOMMAND | (show_tin_ ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(layer_menu_, ID_LAYER_GRID,
                      MF_BYCOMMAND | (show_grid_ ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(layer_menu_, ID_LAYER_CONTOURS,
                      MF_BYCOMMAND | (show_contours_ ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(layer_menu_, ID_LAYER_CDT,
                      MF_BYCOMMAND | (show_cdt_ ? MF_CHECKED : MF_UNCHECKED));
        DrawMenuBar(hwnd_);
    }

    void layout_controls() {
        int x = 6;
        for (size_t i = 0; i < controls_.size(); ++i) {
            const int width = i < 2 ? 92 : (i >= 8 ? 90 : 78);
            MoveWindow(controls_[i], x, 8, width, 30, TRUE);
            x += width + 5;
        }
    }

    void set_wait_cursor(bool waiting) {
        SetCursor(LoadCursorW(nullptr, waiting ? IDC_WAIT : IDC_ARROW));
    }

    void clear_profile() {
        profile_source_ = ProfileSource::None;
        profile_has_start_ = false;
        profile_complete_ = false;
        profile_start_ = {};
        profile_end_ = {};
        profile_samples_.clear();
        profile_statistics_ = {};
    }

    void clear_profile_for_source(ProfileSource source) {
        if (profile_source_ == source) clear_profile();
    }

    void clear_measurement() {
        measurement_source_ = ProfileSource::None;
        measurement_complete_ = false;
        measurement_polygon_.clear();
        measurement_statistics_ = {};
    }

    void clear_measurement_for_source(ProfileSource source) {
        if (measurement_source_ == source) clear_measurement();
    }

    void clear_slope_analysis() {
        slope_source_ = ProfileSource::None;
        slope_valid_ = false;
        slope_analysis_ = {};
    }

    void clear_slope_for_source(ProfileSource source) {
        if (slope_source_ == source) clear_slope_analysis();
    }

    void clear_analysis_for_source(ProfileSource source) {
        clear_profile_for_source(source);
        clear_measurement_for_source(source);
        clear_slope_for_source(source);
    }

    void clear_analysis() {
        clear_profile();
        clear_measurement();
        clear_slope_analysis();
    }

    void destroy_grid_layer() {
        cancel_grid_preview_tasks(false);
        destroy_grid_view_cache();
        clear_analysis_for_source(ProfileSource::Grid);
        dt_grid_destroy(terrain_grid_);
        terrain_grid_ = nullptr;
        dt_grid_destroy(design_grid_);
        design_grid_ = nullptr;
        earthwork_result_ = {};
        earthwork_result_valid_ = false;
        grid_theme_ = GridTheme::Elevation;
        dt_grid_destroy(grid_);
        grid_ = nullptr;
        grid_info_ = {};
        grid_preview_.clear();
        grid_preview_width_ = grid_preview_height_ = 0;
        grid_preview_column_ = grid_preview_row_ = 0;
        grid_preview_source_width_ = grid_preview_source_height_ = 0;
        grid_preview_source_ = nullptr;
        grid_preview_view_valid_ = false;
        grid_preview_used_pyramid_ = false;
        grid_preview_used_tile_cache_ = false;
        grid_preview_used_disk_cache_ = false;
        grid_preview_lod_scale_ = 1;
        grid_preview_tile_count_ = 0;
        grid_preview_reused_tile_count_ = 0;
        grid_preview_theme_ = GridTheme::Elevation;
        grid_disk_cache_file_.clear();
        grid_disk_cache_revision_ = 0;
    }

    void destroy_contour_layer() {
        dt_contours_destroy(contours_);
        contours_ = nullptr;
        contour_info_ = {};
    }

    void clear_derived_layers(bool clear_grid = true,
                              bool clear_contours = true) {
        clear_analysis_for_source(ProfileSource::Tin);
        if (clear_grid) destroy_grid_layer();
        if (clear_contours) destroy_contour_layer();
    }

    void clear_cdt_layer() {
        clear_analysis_for_source(ProfileSource::Cdt);
        if (cdt_) dt_cdt_clear(cdt_);
        cdt_triangles_.clear();
        constraint_lines_.clear();
        cdt_draft_.clear();
        reset_cdt_move_selection();
        cdt_info_ = {};
    }

    void generate(uint64_t count) {
        set_wait_cursor(true);
        std::vector<dt_point3> points;
        points.reserve(static_cast<size_t>(count));
        const uint64_t side = static_cast<uint64_t>(std::ceil(std::sqrt(
            static_cast<double>(count))));
        constexpr double spacing = 10.0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t ix = i % side;
            const uint64_t iy = i / side;
            const double x = static_cast<double>(ix) * spacing + jitter(i * 2 + 1) * 2.0;
            const double y = static_cast<double>(iy) * spacing + jitter(i * 2 + 2) * 2.0;
            points.push_back({x, y, terrain_z(x, y)});
        }
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_build(mesh_, points.data(), points.size(), nullptr);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"建网失败：" + last_error_text();
            return;
        }
        show_tin_ = true;
        update_layer_menu();
        clear_derived_layers();
        clear_cdt_layer();
        clear_overlays();
        reset_view();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"模拟数据建网完成，" << count << L" 点，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
        invalidate_mesh_cache();
    }

    void clear_mesh() {
        if (dt_clear_handle(mesh_) == DT_OK) {
            clear_derived_layers();
            clear_cdt_layer();
            triangles_.clear();
            clear_overlays();
            view_valid_ = false;
            cache_valid_ = false;
            camera_needs_reset_ = true;
            action_text_ = L"三角网已清空";
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void fit_view_to_bounds(const dt_bounds2& bounds, double padding) {
        double width = std::max(bounds.xmax - bounds.xmin, 1.0e-12);
        double height = std::max(bounds.ymax - bounds.ymin, 1.0e-12);
        const RECT canvas = canvas_rect();
        const double aspect = static_cast<double>(std::max(1L, canvas.right - canvas.left)) /
                              static_cast<double>(std::max(1L, canvas.bottom - canvas.top));
        const double center_x = (bounds.xmin + bounds.xmax) * 0.5;
        const double center_y = (bounds.ymin + bounds.ymax) * 0.5;
        width *= padding;
        height *= padding;
        if (width / height > aspect) height = width / aspect;
        else width = height * aspect;
        view_ = {center_x - width * 0.5, center_y - height * 0.5,
                 center_x + width * 0.5, center_y + height * 0.5};
        view_valid_ = true;
        cache_valid_ = false;
        grid_preview_view_valid_ = false;
    }

    void reset_view() {
        bool found = false;
        dt_bounds2 bounds{};
        auto include = [&](const dt_bounds2& next) {
            if (!found) {
                bounds = next;
                found = true;
            } else {
                bounds.xmin = std::min(bounds.xmin, next.xmin);
                bounds.ymin = std::min(bounds.ymin, next.ymin);
                bounds.xmax = std::max(bounds.xmax, next.xmax);
                bounds.ymax = std::max(bounds.ymax, next.ymax);
            }
        };
        dt_statistics stats{};
        if (show_tin_ && dt_get_statistics(mesh_, &stats) == DT_OK &&
            stats.vertex_count != 0) include(stats.bounds);
        if (view_mode_ == ViewMode::Map2D && show_grid_ && grid_) {
            grid_info_.struct_size = sizeof(grid_info_);
            if (dt_grid_get_info(grid_, &grid_info_) == DT_OK)
                include(grid_info_.bounds);
        }
        if (view_mode_ == ViewMode::Map2D && show_contours_ && contours_) {
            contour_info_.struct_size = sizeof(contour_info_);
            if (dt_contours_get_info(contours_, &contour_info_) == DT_OK &&
                contour_info_.line_count != 0) include(contour_info_.bounds);
        }
        if (view_mode_ == ViewMode::Map2D && show_cdt_ && cdt_) {
            dt_cdt_statistics cdt_stats{};
            cdt_stats.struct_size = sizeof(cdt_stats);
            if (dt_cdt_get_statistics(cdt_, &cdt_stats) == DT_OK &&
                cdt_stats.vertex_count != 0) include(cdt_stats.bounds);
        }
        if (!found) {
            view_valid_ = false;
            return;
        }
        fit_view_to_bounds(bounds, 1.08);
        camera_needs_reset_ = true;
    }

    void invalidate_mesh_cache() {
        cache_valid_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void invalidate_grid_view_cache() {
        grid_preview_view_valid_ = false;
    }

    void clear_overlays() {
        removed_triangles_.clear();
        added_triangles_.clear();
        boundary_edges_.clear();
        added_edges_.clear();
        has_highlight_vertex_ = false;
        has_highlight_triangle_ = false;
    }

    void copy_effect(dt_edit_result effect) {
        clear_analysis();
        clear_overlays();
        if (!effect) return;
        dt_edit_result_view view{};
        if (dt_edit_result_get_view(effect, &view) != DT_OK) return;
        auto copy_triangles = [](const dt_triangle3* source, uint64_t count,
                                 std::vector<dt_triangle3>& destination) {
            if (!source || count == 0) return;
            const uint64_t step = std::max<uint64_t>(
                1, (count + kMaxDrawTriangles - 1) / kMaxDrawTriangles);
            destination.reserve(static_cast<size_t>((count + step - 1) / step));
            for (uint64_t i = 0; i < count; i += step)
                destination.push_back(source[i]);
        };
        auto copy_segments = [](const dt_segment3* source, uint64_t count,
                                std::vector<dt_segment3>& destination) {
            if (!source || count == 0) return;
            constexpr uint64_t kMaxDrawSegments = kMaxDrawTriangles * 2;
            const uint64_t step = std::max<uint64_t>(
                1, (count + kMaxDrawSegments - 1) / kMaxDrawSegments);
            destination.reserve(static_cast<size_t>((count + step - 1) / step));
            for (uint64_t i = 0; i < count; i += step)
                destination.push_back(source[i]);
        };
        copy_triangles(view.removed_triangles, view.removed_triangle_count,
                       removed_triangles_);
        copy_triangles(view.added_triangles, view.added_triangle_count,
                       added_triangles_);
        copy_segments(view.boundary_edges, view.boundary_edge_count,
                      boundary_edges_);
        copy_segments(view.added_edges, view.added_edge_count, added_edges_);
    }

    void reset_cdt_move_selection() {
        cdt_move_constraint_id_ = 0;
        cdt_move_point_index_ = std::numeric_limits<size_t>::max();
    }

    bool is_cdt_draw_mode() const {
        return mode_ == Mode::CdtBreakline || mode_ == Mode::CdtOuter ||
               mode_ == Mode::CdtHole;
    }

    void begin_cdt_draft(Mode mode, int32_t kind, const wchar_t* label) {
        enter_2d_view();
        cancel_box_zoom();
        clear_overlays();
        cdt_draft_.clear();
        reset_cdt_move_selection();
        cdt_draft_kind_ = kind;
        mode_ = mode;
        show_cdt_ = true;
        update_layer_menu();
        action_text_ = std::wstring(L"正在绘制") + label +
                       L"：左键逐点，Enter 完成，Backspace 撤点，Esc 取消";
    }

    double interpolate_triangle(const dt_triangle3& triangle,
                                const dt_point3& point) const {
        const auto& a = triangle.vertex[0].point;
        const auto& b = triangle.vertex[1].point;
        const auto& c = triangle.vertex[2].point;
        const double denominator =
            (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (denominator == 0.0) return terrain_z(point.x, point.y);
        const double wa = ((b.y - c.y) * (point.x - c.x) +
                           (c.x - b.x) * (point.y - c.y)) / denominator;
        const double wb = ((c.y - a.y) * (point.x - c.x) +
                           (a.x - c.x) * (point.y - c.y)) / denominator;
        return wa * a.z + wb * b.z + (1.0 - wa - wb) * c.z;
    }

    double constraint_point_z(const dt_point3& point) const {
        double z = 0.0;
        if (cdt_ && dt_cdt_sample_height_xy(cdt_, &point, &z) == DT_OK)
            return z;
        dt_location_result location{};
        if (dt_locate_point_xy(mesh_, &point, &location) == DT_OK) {
            if (location.type == DT_LOCATION_VERTEX)
                return location.vertex.point.z;
            if (location.type == DT_LOCATION_FACE)
                return interpolate_triangle(location.triangle, point);
            if (location.type == DT_LOCATION_EDGE &&
                location.edge.vertex[0].id != 0 &&
                location.edge.vertex[1].id != 0) {
                const auto& a = location.edge.vertex[0].point;
                const auto& b = location.edge.vertex[1].point;
                const double dx = b.x - a.x;
                const double dy = b.y - a.y;
                const double denominator = dx * dx + dy * dy;
                if (denominator > 0.0) {
                    const double t = std::clamp(
                        ((point.x - a.x) * dx + (point.y - a.y) * dy) /
                            denominator,
                        0.0, 1.0);
                    return a.z + t * (b.z - a.z);
                }
            }
        }
        return terrain_z(point.x, point.y);
    }

    void append_cdt_draft_point(const dt_point3& world) {
        dt_point3 point{world.x, world.y, constraint_point_z(world)};
        if (!cdt_draft_.empty() && cdt_draft_.back().x == point.x &&
            cdt_draft_.back().y == point.y) {
            action_text_ = L"该位置与约束草图最后一点重合";
            return;
        }
        cdt_draft_.push_back(point);
        std::wostringstream text;
        text << L"约束草图已取 " << cdt_draft_.size()
             << L" 点；Enter 完成，Backspace 撤点，Esc 取消";
        action_text_ = text.str();
    }

    void finish_cdt_draft() {
        if (!is_cdt_draw_mode()) {
            action_text_ = L"当前没有正在绘制的约束";
            return;
        }
        const size_t minimum = cdt_draft_kind_ == DT_CONSTRAINT_BREAKLINE ? 2 : 3;
        if (cdt_draft_.size() < minimum) {
            std::wostringstream text;
            text << L"当前约束至少需要 " << minimum << L" 个点";
            action_text_ = text.str();
            return;
        }
        dt_constraint_id id = 0;
        set_wait_cursor(true);
        const dt_status status = dt_cdt_add_constraint(
            cdt_, cdt_draft_kind_, 0, cdt_draft_.data(), cdt_draft_.size(), &id);
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"添加约束失败：" + last_error_text();
            return;
        }
        clear_analysis_for_source(ProfileSource::Cdt);
        cdt_draft_.clear();
        destroy_grid_layer();
        destroy_contour_layer();
        refresh_cdt_constraints();
        show_cdt_ = true;
        update_layer_menu();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"约束 ID=" << id << L" 已添加；可继续绘制下一条";
        action_text_ = text.str();
    }

    void add_sample_breaklines_batch() {
        enter_2d_view();
        cdt_draft_.clear();
        reset_cdt_move_selection();
        clear_overlays();
        if (!cdt_ || cdt_info_.vertex_count == 0) {
            action_text_ = L"请先从当前 TIN 创建约束网或打开 DCDT";
            return;
        }
        if (!constraint_lines_.empty()) {
            action_text_ = L"批量示例要求当前约束为空，以避免与已有约束交叉";
            return;
        }
        const double width = cdt_info_.bounds.xmax - cdt_info_.bounds.xmin;
        const double height = cdt_info_.bounds.ymax - cdt_info_.bounds.ymin;
        if (!(width > 0.0) || !(height > 0.0)) {
            action_text_ = L"约束网范围不足，无法生成批量示例";
            return;
        }

        constexpr size_t kLineCount = 12;
        std::vector<std::array<dt_point3, 2>> lines(kLineCount);
        std::vector<dt_cdt_constraint_edit> edits(kLineCount);
        std::vector<dt_constraint_id> ids(kLineCount);
        const double xmin = cdt_info_.bounds.xmin + width * 0.12;
        const double xmax = cdt_info_.bounds.xmin + width * 0.88;
        for (size_t index = 0; index < kLineCount; ++index) {
            const double fraction =
                0.12 + 0.76 * static_cast<double>(index + 1) /
                           static_cast<double>(kLineCount + 1);
            const double y = cdt_info_.bounds.ymin + height * fraction;
            dt_point3 a{xmin, y, 0.0};
            dt_point3 b{xmax, y, 0.0};
            a.z = constraint_point_z(a);
            b.z = constraint_point_z(b);
            lines[index] = {a, b};
            edits[index].struct_size = sizeof(dt_cdt_constraint_edit);
            edits[index].operation = DT_CDT_EDIT_ADD;
            edits[index].kind = DT_CONSTRAINT_BREAKLINE;
            edits[index].points = lines[index].data();
            edits[index].point_count = lines[index].size();
        }

        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_cdt_apply_constraint_edits(
            cdt_, edits.data(), edits.size(), ids.data(), nullptr);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"批量添加示例断裂线失败：" + last_error_text();
            return;
        }
        clear_analysis_for_source(ProfileSource::Cdt);
        destroy_grid_layer();
        destroy_contour_layer();
        refresh_cdt_constraints();
        show_cdt_ = true;
        update_layer_menu();
        reset_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已用一次原子事务添加 " << ids.size()
             << L" 条断裂线，仅重建一次 CDT，耗时 " << std::fixed
             << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void cancel_cdt_draft() {
        if (mode_ == Mode::CdtMoveVertex &&
            cdt_move_constraint_id_ != 0) {
            reset_cdt_move_selection();
            action_text_ = L"已取消约束顶点选择";
            return;
        }
        cdt_draft_.clear();
        action_text_ = L"已取消当前约束草图";
    }

    static double point_segment_distance_squared(POINT point, POINT a,
                                                  POINT b) {
        const double dx = static_cast<double>(b.x - a.x);
        const double dy = static_cast<double>(b.y - a.y);
        const double denominator = dx * dx + dy * dy;
        if (denominator == 0.0) {
            const double px = static_cast<double>(point.x - a.x);
            const double py = static_cast<double>(point.y - a.y);
            return px * px + py * py;
        }
        const double t = std::clamp(
            (static_cast<double>(point.x - a.x) * dx +
             static_cast<double>(point.y - a.y) * dy) /
                denominator,
            0.0, 1.0);
        const double px = static_cast<double>(point.x) -
                          (static_cast<double>(a.x) + t * dx);
        const double py = static_cast<double>(point.y) -
                          (static_cast<double>(a.y) + t * dy);
        return px * px + py * py;
    }

    void move_cdt_vertex_at(int x, int y, const dt_point3& world) {
        if (constraint_lines_.empty()) {
            action_text_ = L"当前没有可编辑的约束顶点";
            return;
        }
        constexpr double kPickRadius = 14.0;
        if (cdt_move_constraint_id_ == 0) {
            const POINT clicked{x, y};
            double best_distance = std::numeric_limits<double>::infinity();
            for (const auto& line : constraint_lines_) {
                for (size_t index = 0; index < line.points.size(); ++index) {
                    const POINT point = world_to_screen(line.points[index]);
                    const double dx = static_cast<double>(clicked.x - point.x);
                    const double dy = static_cast<double>(clicked.y - point.y);
                    const double distance = dx * dx + dy * dy;
                    if (distance < best_distance) {
                        best_distance = distance;
                        cdt_move_constraint_id_ = line.id;
                        cdt_move_point_index_ = index;
                    }
                }
            }
            if (best_distance > kPickRadius * kPickRadius) {
                reset_cdt_move_selection();
                action_text_ = L"未选中约束顶点，请在彩色约束折点附近单击";
                return;
            }
            std::wostringstream text;
            text << L"已选择约束 ID=" << cdt_move_constraint_id_
                 << L" 的第 " << (cdt_move_point_index_ + 1)
                 << L" 个点；单击新位置，Esc 取消";
            action_text_ = text.str();
            return;
        }

        const auto line = std::find_if(
            constraint_lines_.begin(), constraint_lines_.end(),
            [&](const ConstraintLine& item) {
                return item.id == cdt_move_constraint_id_;
            });
        if (line == constraint_lines_.end() ||
            cdt_move_point_index_ >= line->points.size()) {
            reset_cdt_move_selection();
            action_text_ = L"所选约束已变化，请重新选择顶点";
            return;
        }
        auto points = line->points;
        points[cdt_move_point_index_] =
            {world.x, world.y, constraint_point_z(world)};
        const dt_constraint_id moved_id = line->id;
        const size_t moved_index = cdt_move_point_index_;
        dt_edit_result effect = nullptr;
        set_wait_cursor(true);
        const dt_status status = dt_cdt_update_constraint(
            cdt_, line->id, line->flags, points.data(), points.size(), &effect);
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"移动约束顶点失败：" + last_error_text() +
                           L"；可重新选择位置或按 Esc 取消";
            dt_release_edit_result(effect);
            return;
        }
        uint64_t removed_count = 0;
        uint64_t added_count = 0;
        dt_edit_result_view effect_view{};
        if (dt_edit_result_get_view(effect, &effect_view) == DT_OK) {
            removed_count = effect_view.removed_triangle_count;
            added_count = effect_view.added_triangle_count;
        }
        copy_effect(effect);
        dt_release_edit_result(effect);
        reset_cdt_move_selection();
        destroy_grid_layer();
        destroy_contour_layer();
        refresh_cdt_constraints();
        show_cdt_ = true;
        update_layer_menu();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"已移动约束 ID=" << moved_id << L" 的第 "
             << (moved_index + 1) << L" 个点；影响旧面 " << removed_count
             << L"，新面 " << added_count << L"；可继续选择下一顶点";
        action_text_ = text.str();
    }

    void remove_cdt_vertex_at(int x, int y) {
        if (constraint_lines_.empty()) {
            action_text_ = L"当前没有可删除的约束顶点";
            return;
        }
        const POINT clicked{x, y};
        double best_distance = std::numeric_limits<double>::infinity();
        dt_constraint_id best_id = 0;
        size_t best_index = std::numeric_limits<size_t>::max();
        for (const auto& line : constraint_lines_) {
            for (size_t index = 0; index < line.points.size(); ++index) {
                const POINT point = world_to_screen(line.points[index]);
                const double dx = static_cast<double>(clicked.x - point.x);
                const double dy = static_cast<double>(clicked.y - point.y);
                const double distance = dx * dx + dy * dy;
                if (distance < best_distance) {
                    best_distance = distance;
                    best_id = line.id;
                    best_index = index;
                }
            }
        }
        constexpr double kPickRadius = 14.0;
        if (best_id == 0 || best_distance > kPickRadius * kPickRadius) {
            action_text_ = L"未选中约束顶点，请在白色折点附近单击";
            return;
        }

        dt_cdt_vertex_usage usage{};
        if (dt_cdt_get_constraint_vertex_usage(cdt_, best_id, best_index,
                                               &usage) != DT_OK) {
            action_text_ = L"查询约束顶点引用失败：" + last_error_text();
            return;
        }
        uint32_t flags = 0;
        if (usage.constraint_count > 1) {
            std::wostringstream prompt;
            prompt << L"该顶点被 " << usage.constraint_count
                   << L" 条约束共同引用。\n\n"
                   << L"是否仅从约束 ID=" << best_id
                   << L" 中脱离此顶点？\n"
                   << L"其他约束将保持不变。";
            if (MessageBoxW(hwnd_, prompt.str().c_str(),
                            L"共享约束顶点保护",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                action_text_ = L"已取消删除共享约束顶点";
                return;
            }
            flags |= DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH;
        }

        dt_edit_result effect = nullptr;
        set_wait_cursor(true);
        const dt_status status = dt_cdt_remove_constraint_vertex(
            cdt_, best_id, best_index, flags, &effect);
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"删除约束顶点失败：" + last_error_text();
            dt_release_edit_result(effect);
            return;
        }
        uint64_t removed_count = 0;
        uint64_t added_count = 0;
        dt_edit_result_view effect_view{};
        if (dt_edit_result_get_view(effect, &effect_view) == DT_OK) {
            removed_count = effect_view.removed_triangle_count;
            added_count = effect_view.added_triangle_count;
        }
        copy_effect(effect);
        dt_release_edit_result(effect);
        destroy_grid_layer();
        destroy_contour_layer();
        refresh_cdt_constraints();
        show_cdt_ = true;
        update_layer_menu();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"已删除约束 ID=" << best_id << L" 的第 "
             << (best_index + 1) << L" 个点；影响旧面 " << removed_count
             << L"，新面 " << added_count;
        if (usage.constraint_count > 1) text << L"；其他共享约束保持不变";
        if (usage.is_base_point != 0) text << L"；基础地形点仍保留";
        action_text_ = text.str();
    }

    void delete_cdt_constraint_at(int x, int y) {
        if (constraint_lines_.empty()) {
            action_text_ = L"当前没有可删除的约束";
            return;
        }
        const POINT clicked{x, y};
        double best_distance = std::numeric_limits<double>::infinity();
        dt_constraint_id best_id = 0;
        for (const auto& line : constraint_lines_) {
            if (line.points.size() < 2) continue;
            const size_t segment_count = line.points.size() - 1 +
                (((line.flags & DT_CONSTRAINT_CLOSED) != 0) ? 1 : 0);
            for (size_t segment = 0; segment < segment_count; ++segment) {
                const POINT a = world_to_screen(line.points[segment % line.points.size()]);
                const POINT b = world_to_screen(
                    line.points[(segment + 1) % line.points.size()]);
                const double distance = point_segment_distance_squared(clicked, a, b);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_id = line.id;
                }
            }
        }
        constexpr double kPickRadius = 14.0;
        if (best_id == 0 || best_distance > kPickRadius * kPickRadius) {
            action_text_ = L"未选中约束，请在彩色约束线附近单击";
            return;
        }
        set_wait_cursor(true);
        const dt_status status = dt_cdt_remove_constraint(cdt_, best_id);
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"删除约束失败：" + last_error_text();
            return;
        }
        clear_analysis_for_source(ProfileSource::Cdt);
        destroy_grid_layer();
        destroy_contour_layer();
        refresh_cdt_constraints();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"已删除约束 ID=" << best_id;
        action_text_ = text.str();
    }

    static const wchar_t* surface_source_name(ProfileSource source) {
        switch (source) {
        case ProfileSource::Cdt: return L"约束网有效域";
        case ProfileSource::Tin: return L"TIN";
        case ProfileSource::Grid: return L"GRID";
        default: return L"无";
        }
    }

    const wchar_t* profile_source_name() const {
        return surface_source_name(profile_source_);
    }

    ProfileSource select_profile_source() const {
        dt_cdt_statistics cdt_stats{};
        cdt_stats.struct_size = sizeof(cdt_stats);
        const bool has_cdt = cdt_ &&
            dt_cdt_get_statistics(cdt_, &cdt_stats) == DT_OK &&
            cdt_stats.domain_triangle_count != 0;
        dt_statistics tin_stats{};
        const bool has_tin = dt_get_statistics(mesh_, &tin_stats) == DT_OK &&
                             tin_stats.dimension == 2;
        if (show_cdt_ && has_cdt) return ProfileSource::Cdt;
        if (show_tin_ && has_tin) return ProfileSource::Tin;
        if (show_grid_ && grid_) return ProfileSource::Grid;
        if (has_cdt) return ProfileSource::Cdt;
        if (has_tin) return ProfileSource::Tin;
        if (grid_) return ProfileSource::Grid;
        return ProfileSource::None;
    }

    bool sample_tin_profile(const dterrain::profile::Point& point,
                            double& output_z) const {
        dt_point3 query{point.x, point.y, 0.0};
        dt_location_result location{};
        if (dt_locate_point_xy(mesh_, &query, &location) != DT_OK) return false;
        if (location.type == DT_LOCATION_VERTEX) {
            output_z = location.vertex.point.z;
            return std::isfinite(output_z);
        }
        if (location.type == DT_LOCATION_EDGE) {
            const auto& edge = location.edge;
            if (edge.vertex[0].id == 0 || edge.vertex[1].id == 0) return false;
            return dterrain::profile::segment_height(
                point, {edge.vertex[0].point.x, edge.vertex[0].point.y},
                edge.vertex[0].point.z,
                {edge.vertex[1].point.x, edge.vertex[1].point.y},
                edge.vertex[1].point.z, output_z);
        }
        if (location.type == DT_LOCATION_FACE) {
            const auto& triangle = location.triangle;
            if (triangle.vertex[0].id == 0 || triangle.vertex[1].id == 0 ||
                triangle.vertex[2].id == 0) {
                return false;
            }
            return dterrain::profile::triangle_height(
                point,
                {triangle.vertex[0].point.x, triangle.vertex[0].point.y},
                triangle.vertex[0].point.z,
                {triangle.vertex[1].point.x, triangle.vertex[1].point.y},
                triangle.vertex[1].point.z,
                {triangle.vertex[2].point.x, triangle.vertex[2].point.y},
                triangle.vertex[2].point.z, output_z);
        }
        return false;
    }

    bool sample_grid_profile(const dterrain::profile::Point& point,
                             double& output_z) {
        if (!grid_) return false;
        grid_info_ = {};
        grid_info_.struct_size = sizeof(grid_info_);
        if (dt_grid_get_info(grid_, &grid_info_) != DT_OK ||
            grid_info_.width == 0 || grid_info_.height == 0) {
            return false;
        }
        double column = 0.0;
        double row = 0.0;
        if (!dterrain::profile::grid_coordinates(
                grid_info_.geo_transform, point, column, row)) {
            return false;
        }
        constexpr double tolerance = 1.0e-9;
        if (column < -tolerance || row < -tolerance ||
            column > static_cast<double>(grid_info_.width - 1) + tolerance ||
            row > static_cast<double>(grid_info_.height - 1) + tolerance) {
            return false;
        }
        column = std::clamp(column, 0.0,
                            static_cast<double>(grid_info_.width - 1));
        row = std::clamp(row, 0.0,
                         static_cast<double>(grid_info_.height - 1));
        const uint64_t column0 = static_cast<uint64_t>(std::floor(column));
        const uint64_t row0 = static_cast<uint64_t>(std::floor(row));
        const uint64_t column1 = std::min(column0 + 1, grid_info_.width - 1);
        const uint64_t row1 = std::min(row0 + 1, grid_info_.height - 1);
        const uint64_t width = column1 - column0 + 1;
        const uint64_t height = row1 - row0 + 1;
        double values[4]{};
        if (dt_grid_read_window(grid_, column0, row0, width, height,
                                values, width) != DT_OK) {
            return false;
        }
        auto value = [&](uint64_t x, uint64_t y) {
            return values[static_cast<size_t>(y * width + x)];
        };
        const double z00 = value(0, 0);
        const double z10 = value(width - 1, 0);
        const double z01 = value(0, height - 1);
        const double z11 = value(width - 1, height - 1);
        if (is_grid_nodata(z00) || is_grid_nodata(z10) ||
            is_grid_nodata(z01) || is_grid_nodata(z11)) {
            return false;
        }
        output_z = dterrain::profile::bilinear(
            z00, z10, z01, z11, column - static_cast<double>(column0),
            row - static_cast<double>(row0));
        return std::isfinite(output_z);
    }

    bool sample_surface_point(ProfileSource source,
                              const dterrain::profile::Point& point,
                              double& output_z) {
        if (source == ProfileSource::Cdt) {
            dt_point3 query{point.x, point.y, 0.0};
            return dt_cdt_sample_height_xy(cdt_, &query, &output_z) == DT_OK;
        }
        if (source == ProfileSource::Tin)
            return sample_tin_profile(point, output_z);
        if (source == ProfileSource::Grid)
            return sample_grid_profile(point, output_z);
        return false;
    }

    static const wchar_t* aspect_direction(double degrees) {
        static const wchar_t* directions[8] = {
            L"北", L"东北", L"东", L"东南",
            L"南", L"西南", L"西", L"西北"};
        const int index = static_cast<int>(
            std::floor((degrees + 22.5) / 45.0)) % 8;
        return directions[index];
    }

    void analyze_slope_at(const dt_point3& world) {
        const ProfileSource source = select_profile_source();
        if (source == ProfileSource::None) {
            clear_slope_analysis();
            action_text_ = L"坡度/坡向分析失败：当前没有可分析地形表面";
            return;
        }
        dt_point3 query{world.x, world.y, 0.0};
        dt_surface_analysis analysis{};
        dt_status status = DT_E_NOT_FOUND;
        if (source == ProfileSource::Cdt)
            status = dt_cdt_analyze_surface_xy(cdt_, &query, &analysis);
        else if (source == ProfileSource::Tin)
            status = dt_analyze_tin_surface_xy(mesh_, &query, &analysis);
        else if (source == ProfileSource::Grid)
            status = dt_grid_analyze_surface_xy(grid_, &query, &analysis);
        if (status != DT_OK) {
            clear_slope_analysis();
            action_text_ = L"坡度/坡向分析失败：" + last_error_text();
            return;
        }
        slope_source_ = source;
        slope_analysis_ = analysis;
        slope_valid_ = true;
        std::wostringstream text;
        text << L"坡面分析（" << surface_source_name(source) << L"）：Z="
             << std::fixed << std::setprecision(3) << analysis.point.z
             << L"，坡度=" << analysis.slope_degrees << L"°";
        if ((analysis.flags & DT_SURFACE_ASPECT_UNDEFINED) != 0) {
            text << L"，水平面无唯一坡向";
        } else {
            text << L"，坡向=" << analysis.aspect_degrees << L"°（"
                 << aspect_direction(analysis.aspect_degrees) << L"）";
        }
        action_text_ = text.str();
    }

    bool sample_profile_point(const dterrain::profile::Point& point,
                              double& output_z) {
        return sample_surface_point(profile_source_, point, output_z);
    }

    bool calculate_profile() {
        profile_source_ = select_profile_source();
        if (profile_source_ == ProfileSource::None) {
            action_text_ = L"剖面分析失败：当前没有可采样的 TIN、GRID 或 CDT";
            return false;
        }
        const double dx = profile_end_.x - profile_start_.x;
        const double dy = profile_end_.y - profile_start_.y;
        const double distance = std::hypot(dx, dy);
        const double coordinate_scale = std::max(
            {1.0, std::abs(profile_start_.x), std::abs(profile_start_.y),
             std::abs(profile_end_.x), std::abs(profile_end_.y)});
        if (!(distance > std::numeric_limits<double>::epsilon() *
                         coordinate_scale * 32.0)) {
            action_text_ = L"剖面终点与起点过近，请重新单击终点";
            return false;
        }
        constexpr size_t kSampleCount = 401;
        const auto points = dterrain::profile::line_points(
            {profile_start_.x, profile_start_.y},
            {profile_end_.x, profile_end_.y}, kSampleCount);
        profile_samples_.clear();
        profile_samples_.reserve(points.size());
        set_wait_cursor(true);
        for (size_t index = 0; index < points.size(); ++index) {
            dterrain::profile::Sample sample{};
            sample.distance = distance * static_cast<double>(index) /
                              static_cast<double>(points.size() - 1);
            sample.x = points[index].x;
            sample.y = points[index].y;
            sample.valid = sample_profile_point(points[index], sample.z);
            profile_samples_.push_back(sample);
        }
        set_wait_cursor(false);
        profile_statistics_ = dterrain::profile::summarize(profile_samples_);
        if (profile_statistics_.valid_count < 2) {
            profile_samples_.clear();
            profile_statistics_ = {};
            action_text_ = L"剖面分析失败：选线在当前采样表面上没有足够有效点";
            return false;
        }
        profile_complete_ = true;
        if (profile_samples_.front().valid)
            profile_start_.z = profile_samples_.front().z;
        if (profile_samples_.back().valid)
            profile_end_.z = profile_samples_.back().z;
        std::wostringstream text;
        text << L"剖面完成（" << profile_source_name() << L"）：距离 "
             << std::fixed << std::setprecision(2)
             << profile_statistics_.horizontal_distance << L"，有效 "
             << profile_statistics_.valid_count << L"/" << kSampleCount
             << L"，高程 " << profile_statistics_.minimum_z << L"～"
             << profile_statistics_.maximum_z;
        if (std::isfinite(profile_statistics_.elevation_difference))
            text << L"，高差 " << profile_statistics_.elevation_difference;
        text << L"，最大分段坡度 " << std::setprecision(1)
             << profile_statistics_.maximum_absolute_grade_percent << L"%";
        action_text_ = text.str();
        return true;
    }

    void profile_click(const dt_point3& world) {
        if (!profile_has_start_ || profile_complete_) {
            clear_profile();
            profile_has_start_ = true;
            profile_start_ = world;
            action_text_ = L"剖面起点已选择，请单击终点；Esc 清除";
            return;
        }
        profile_end_ = world;
        calculate_profile();
    }

    void export_profile_csv() {
        if (!profile_complete_ || profile_samples_.empty()) {
            action_text_ = L"没有可导出的剖面，请先完成两次单击";
            return;
        }
        const auto file = choose_file(
            true, L"terrain_profile.csv",
            L"CSV 剖面 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0", L"csv");
        if (file.empty()) return;
        std::ostringstream output;
        output << std::setprecision(17);
        output << "# source," << wide_to_utf8(profile_source_name()) << "\r\n";
        output << "index,distance,x,y,z,valid,grade_percent\r\n";
        for (size_t index = 0; index < profile_samples_.size(); ++index) {
            const auto& sample = profile_samples_[index];
            output << index << ',' << sample.distance << ',' << sample.x << ','
                   << sample.y << ',';
            if (sample.valid && std::isfinite(sample.z)) output << sample.z;
            output << ',' << (sample.valid ? 1 : 0) << ',';
            if (index != 0 && sample.valid &&
                profile_samples_[index - 1].valid) {
                const double run = sample.distance -
                                   profile_samples_[index - 1].distance;
                if (run > 0.0)
                    output << (sample.z - profile_samples_[index - 1].z) /
                                  run * 100.0;
            }
            output << "\r\n";
        }
        const std::string bytes = output.str();
        FILE* stream = _wfopen(file.c_str(), L"wb");
        if (!stream) {
            action_text_ = L"剖面 CSV 导出失败：无法创建文件";
            return;
        }
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), stream);
        const int close_status = std::fclose(stream);
        if (written != bytes.size() || close_status != 0) {
            action_text_ = L"剖面 CSV 导出失败：文件写入不完整";
            return;
        }
        action_text_ = L"已导出剖面 CSV：" + file;
    }

    bool calculate_measurement() {
        if (measurement_polygon_.size() < 3) {
            action_text_ = L"面积/土方量测至少需要 3 个多边形点";
            return false;
        }
        std::vector<dterrain::measurement::Point> polygon;
        polygon.reserve(measurement_polygon_.size());
        for (const auto& point : measurement_polygon_)
            polygon.push_back({point.x, point.y});
        if (!dterrain::measurement::is_simple_polygon(polygon)) {
            action_text_ = L"量测失败：多边形自交、退化或含有重复相邻点";
            return false;
        }
        if (measurement_source_ == ProfileSource::None)
            measurement_source_ = select_profile_source();
        if (measurement_source_ == ProfileSource::None) {
            action_text_ = L"量测失败：当前没有可采样的 TIN、GRID 或 CDT";
            return false;
        }
        set_wait_cursor(true);
        measurement_statistics_ = dterrain::measurement::integrate_polygon(
            polygon, measurement_datum_z_, 20000,
            [this](const dterrain::measurement::Point& point, double& z) {
                return sample_surface_point(measurement_source_, point, z);
            });
        set_wait_cursor(false);
        if (!(measurement_statistics_.polygon.area > 0.0) ||
            !(measurement_statistics_.valid_plan_area > 0.0)) {
            measurement_statistics_ = {};
            action_text_ = L"量测失败：多边形在固定数据源内没有有效覆盖面积";
            return false;
        }
        measurement_complete_ = true;
        const double coverage = measurement_statistics_.valid_plan_area /
                                measurement_statistics_.polygon.area * 100.0;
        std::wostringstream text;
        text << L"面积/土方完成（"
             << surface_source_name(measurement_source_) << L"）：投影面积 "
             << std::fixed << std::setprecision(2)
             << measurement_statistics_.polygon.area << L"，有效覆盖 "
             << coverage << L"%，地表面积 "
             << measurement_statistics_.surface_area << L"，挖/填方 "
             << measurement_statistics_.cut_volume << L"/"
             << measurement_statistics_.fill_volume << L"（基准 Z="
             << measurement_datum_z_ << L"）";
        action_text_ = text.str();
        return true;
    }

    void begin_measurement_mode() {
        double datum = measurement_datum_z_;
        if (!prompt_double(hwnd_, L"面积/土方量测",
                           L"请输入水平设计基准高程 Z：", datum)) {
            action_text_ = L"已取消面积/土方量测";
            return;
        }
        enter_2d_view();
        cdt_draft_.clear();
        reset_cdt_move_selection();
        cancel_box_zoom();
        clear_measurement();
        measurement_datum_z_ = datum;
        mode_ = Mode::Measure;
        action_text_ = L"逐点单击量测多边形；Enter 完成，Backspace 撤点，Esc 清除";
    }

    void measurement_click(const dt_point3& world) {
        if (measurement_complete_) clear_measurement();
        if (measurement_polygon_.empty()) {
            measurement_source_ = select_profile_source();
            if (measurement_source_ == ProfileSource::None) {
                action_text_ = L"量测失败：当前没有可采样地形表面";
                return;
            }
        }
        if (!measurement_polygon_.empty()) {
            const auto& previous = measurement_polygon_.back();
            const double scale = std::max(
                {1.0, std::abs(previous.x), std::abs(previous.y),
                 std::abs(world.x), std::abs(world.y)});
            if (std::hypot(world.x - previous.x, world.y - previous.y) <=
                std::numeric_limits<double>::epsilon() * scale * 32.0) {
                action_text_ = L"量测点与上一点过近，已忽略";
                return;
            }
        }
        dt_point3 point = world;
        if (!sample_surface_point(measurement_source_, {world.x, world.y},
                                  point.z)) {
            point.z = std::numeric_limits<double>::quiet_NaN();
        }
        measurement_polygon_.push_back(point);
        std::wostringstream text;
        text << L"量测多边形已输入 " << measurement_polygon_.size()
             << L" 点；固定数据源="
             << surface_source_name(measurement_source_)
             << L"，Enter 完成";
        action_text_ = text.str();
    }

    void finish_measurement() {
        if (mode_ != Mode::Measure) {
            action_text_ = L"请先选择“面积/土方量测（逐点）”";
            return;
        }
        if (measurement_complete_) {
            action_text_ = L"量测已完成；可导出 CSV、修改基准或单击开始新多边形";
            return;
        }
        calculate_measurement();
    }

    void set_measurement_datum() {
        double datum = measurement_datum_z_;
        if (!prompt_double(hwnd_, L"设置土方基准高程",
                           L"请输入水平设计基准高程 Z：", datum)) {
            return;
        }
        measurement_datum_z_ = datum;
        if (measurement_complete_) {
            measurement_complete_ = false;
            calculate_measurement();
        } else {
            std::wostringstream text;
            text << L"土方基准高程已设置为 Z=" << std::setprecision(15)
                 << datum;
            action_text_ = text.str();
        }
    }

    void export_measurement_csv() {
        if (!measurement_complete_ || measurement_polygon_.empty()) {
            action_text_ = L"没有可导出的面积/土方结果，请先完成多边形量测";
            return;
        }
        const auto file = choose_file(
            true, L"terrain_measurement.csv",
            L"CSV 量测 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0", L"csv");
        if (file.empty()) return;
        std::ostringstream output;
        output << std::setprecision(17);
        output << "# source,"
               << wide_to_utf8(surface_source_name(measurement_source_))
               << "\r\n";
        output << "# datum_z," << measurement_datum_z_ << "\r\n";
        output << "# polygon_area," << measurement_statistics_.polygon.area
               << "\r\n# perimeter," << measurement_statistics_.polygon.perimeter
               << "\r\n# valid_plan_area," << measurement_statistics_.valid_plan_area
               << "\r\n# surface_area," << measurement_statistics_.surface_area
               << "\r\n# minimum_z," << measurement_statistics_.minimum_z
               << "\r\n# maximum_z," << measurement_statistics_.maximum_z
               << "\r\n# mean_z," << measurement_statistics_.mean_z
               << "\r\n# cut_volume," << measurement_statistics_.cut_volume
               << "\r\n# fill_volume," << measurement_statistics_.fill_volume
               << "\r\n# net_cut_volume," << measurement_statistics_.net_cut_volume
               << "\r\n# valid_micro_triangles,"
               << measurement_statistics_.valid_micro_triangle_count
               << "\r\n# total_micro_triangles,"
               << measurement_statistics_.total_micro_triangle_count
               << "\r\n";
        output << "index,x,y,z,valid\r\n";
        for (size_t index = 0; index < measurement_polygon_.size(); ++index) {
            const auto& point = measurement_polygon_[index];
            const bool valid = std::isfinite(point.z);
            output << index << ',' << point.x << ',' << point.y << ',';
            if (valid) output << point.z;
            output << ',' << (valid ? 1 : 0) << "\r\n";
        }
        const std::string bytes = output.str();
        FILE* stream = _wfopen(file.c_str(), L"wb");
        if (!stream) {
            action_text_ = L"量测 CSV 导出失败：无法创建文件";
            return;
        }
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), stream);
        const int close_status = std::fclose(stream);
        if (written != bytes.size() || close_status != 0) {
            action_text_ = L"量测 CSV 导出失败：文件写入不完整";
            return;
        }
        action_text_ = L"已导出面积/土方量测 CSV：" + file;
    }

    void on_left_click(int x, int y) {
        if (!view_valid_) return;
        const dt_point3 world = screen_to_world(x, y);
        const auto begin = std::chrono::steady_clock::now();
        if (mode_ == Mode::Profile) {
            profile_click(world);
        } else if (mode_ == Mode::Measure) {
            measurement_click(world);
        } else if (mode_ == Mode::Slope) {
            analyze_slope_at(world);
        } else if (is_cdt_draw_mode()) {
            append_cdt_draft_point(world);
        } else if (mode_ == Mode::CdtMoveVertex) {
            move_cdt_vertex_at(x, y, world);
        } else if (mode_ == Mode::CdtRemoveVertex) {
            remove_cdt_vertex_at(x, y);
        } else if (mode_ == Mode::CdtDelete) {
            delete_cdt_constraint_at(x, y);
        } else if (mode_ == Mode::Insert) {
            dt_point3 point{world.x, world.y, terrain_z(world.x, world.y)};
            dt_vertex_id id = 0;
            dt_edit_result effect = nullptr;
            const auto status = dt_insert_point(mesh_, &point, &id, &effect);
            if (status == DT_OK) {
                show_tin_ = true;
                update_layer_menu();
                clear_derived_layers();
                copy_effect(effect);
                std::wostringstream text;
                text << L"插入顶点 ID=" << id;
                action_text_ = text.str();
                invalidate_mesh_cache();
            } else action_text_ = L"插入失败：" + last_error_text();
            dt_release_edit_result(effect);
        } else if (mode_ == Mode::Delete) {
            dt_vertex_id id = 0;
            dt_edit_result effect = nullptr;
            const auto status = dt_delete_nearest_xy(mesh_, &world, &id, &effect);
            if (status == DT_OK) {
                show_tin_ = true;
                update_layer_menu();
                clear_derived_layers();
                copy_effect(effect);
                std::wostringstream text;
                text << L"删除最近顶点 ID=" << id;
                action_text_ = text.str();
                invalidate_mesh_cache();
            } else action_text_ = L"删除失败：" + last_error_text();
            dt_release_edit_result(effect);
        } else {
            clear_overlays();
            dt_vertex3 nearest{};
            dt_location_result location{};
            if (dt_find_nearest_vertex_xy(mesh_, &world, &nearest) == DT_OK) {
                highlight_vertex_ = nearest;
                has_highlight_vertex_ = true;
            }
            if (dt_locate_point_xy(mesh_, &world, &location) == DT_OK &&
                (location.type == DT_LOCATION_FACE ||
                 location.type == DT_LOCATION_EDGE)) {
                highlight_triangle_ = location.triangle;
                has_highlight_triangle_ = location.triangle.vertex[0].id != 0;
            }
            std::wostringstream text;
            text << L"查询 XY=(" << std::fixed << std::setprecision(2)
                 << world.x << L", " << world.y << L")";
            if (has_highlight_vertex_)
                text << L"，最近顶点 ID=" << highlight_vertex_.id
                     << L"，Z=" << highlight_vertex_.point.z;
            action_text_ = text.str();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream suffix;
        suffix << action_text_ << L"，操作 " << std::fixed << std::setprecision(3)
               << ms << L" ms";
        action_text_ = suffix.str();
    }

    void begin_pan(int x, int y) {
        if (!view_valid_ || y < kToolbarHeight) return;
        panning_ = true;
        pan_start_ = {x, y};
        pan_view_ = view_;
        SetCapture(hwnd_);
    }

    void pan_to(int x, int y) {
        const RECT canvas = canvas_rect();
        const double sx = (pan_view_.xmax - pan_view_.xmin) /
                          std::max(1L, canvas.right - canvas.left);
        const double sy = (pan_view_.ymax - pan_view_.ymin) /
                          std::max(1L, canvas.bottom - canvas.top);
        const double dx = static_cast<double>(x - pan_start_.x) * sx;
        const double dy = static_cast<double>(y - pan_start_.y) * sy;
        view_ = {pan_view_.xmin - dx, pan_view_.ymin + dy,
                 pan_view_.xmax - dx, pan_view_.ymax + dy};
        invalidate_mesh_cache();
    }

    void end_pan() {
        if (!panning_) return;
        panning_ = false;
        invalidate_grid_view_cache();
        invalidate_mesh_cache();
    }

    void zoom(int x, int y, int delta) {
        if (!view_valid_ || y < kToolbarHeight) return;
        const auto anchor = screen_to_world(x, y);
        const double factor = delta > 0 ? 0.78 : 1.28;
        view_.xmin = anchor.x + (view_.xmin - anchor.x) * factor;
        view_.xmax = anchor.x + (view_.xmax - anchor.x) * factor;
        view_.ymin = anchor.y + (view_.ymin - anchor.y) * factor;
        view_.ymax = anchor.y + (view_.ymax - anchor.y) * factor;
        invalidate_grid_view_cache();
        invalidate_mesh_cache();
    }

    void begin_drag3d(Drag3D drag, int x, int y) {
        if (!view_valid_ || y < kToolbarHeight) return;
        drag3d_ = drag;
        drag3d_last_ = {x, y};
        SetCapture(hwnd_);
    }

    void drag3d_to(int x, int y) {
        const int dx = x - drag3d_last_.x;
        const int dy = y - drag3d_last_.y;
        drag3d_last_ = {x, y};
        if (drag3d_ == Drag3D::Orbit) {
            camera_.orbit(-static_cast<double>(dx) * 0.008,
                          static_cast<double>(dy) * 0.006);
            action_text_ = L"3D 环视：左键拖动，右键平移，滚轮缩放";
        } else if (drag3d_ == Drag3D::Pan) {
            const double scale = camera_.distance * 0.0018;
            dterrain::viewer3d::pan(camera_, -dx * scale, dy * scale);
            action_text_ = L"3D 视点已平移";
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void end_drag3d() {
        drag3d_ = Drag3D::None;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }

    void zoom3d(int delta) {
        if (!view_valid_) return;
        camera_.dolly(delta > 0 ? 0.82 : 1.22);
        action_text_ = L"3D 相机距离已调整";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool handle_key3d(WPARAM key) {
        const double step = std::max(0.025, camera_.distance * 0.035);
        bool handled = true;
        switch (key) {
        case 'W':
        case VK_UP: dterrain::viewer3d::roam_xy(camera_, step, 0.0); break;
        case 'S':
        case VK_DOWN: dterrain::viewer3d::roam_xy(camera_, -step, 0.0); break;
        case 'A':
        case VK_LEFT: dterrain::viewer3d::roam_xy(camera_, 0.0, -step); break;
        case 'D':
        case VK_RIGHT: dterrain::viewer3d::roam_xy(camera_, 0.0, step); break;
        case 'Q': camera_.target.z += step; break;
        case 'E': camera_.target.z -= step; break;
        case VK_HOME: reset_camera3d(); break;
        case VK_ADD:
        case VK_OEM_PLUS: change_exaggeration(1); break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS: change_exaggeration(-1); break;
        case VK_ESCAPE: end_drag3d(); break;
        default: handled = false; break;
        }
        if (handled) {
            action_text_ = L"3D 漫游：WASD/方向键移动，Q/E 升降，Home 复位";
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return handled;
    }

    void toggle_view_mode() {
        end_drag3d();
        cancel_box_zoom();
        if (view_mode_ == ViewMode::Map2D) {
            dt_statistics stats{};
            if (dt_get_statistics(mesh_, &stats) != DT_OK || stats.dimension != 2) {
                action_text_ = L"3D 视图需要有效的二维 TIN；可先执行 GRID→TIN";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            show_tin_ = true;
            update_layer_menu();
            view_mode_ = ViewMode::Terrain3D;
            reset_view();
            SetWindowTextW(view_button_, L"切换2D");
            EnableWindow(exaggeration_button_, TRUE);
            action_text_ = L"已进入 3D：左键环视，右键平移，滚轮缩放，WASD 漫游";
        } else {
            view_mode_ = ViewMode::Map2D;
            reset_view();
            SetWindowTextW(view_button_, L"切换3D");
            EnableWindow(exaggeration_button_, FALSE);
            action_text_ = L"已返回 2D 编辑视图";
        }
        SetFocus(hwnd_);
        invalidate_mesh_cache();
    }

    void change_exaggeration(int direction) {
        constexpr std::array<double, 7> levels{0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0};
        size_t nearest = 0;
        for (size_t i = 1; i < levels.size(); ++i)
            if (std::abs(levels[i] - z_exaggeration_) <
                std::abs(levels[nearest] - z_exaggeration_)) nearest = i;
        if (direction > 0 && nearest + 1 < levels.size()) ++nearest;
        if (direction < 0 && nearest > 0) --nearest;
        z_exaggeration_ = levels[nearest];
        std::wostringstream label;
        label << L"高程×" << std::fixed << std::setprecision(1) << z_exaggeration_;
        SetWindowTextW(exaggeration_button_, label.str().c_str());
        std::wostringstream status;
        status << L"垂直夸张已设为 " << std::fixed << std::setprecision(1)
               << z_exaggeration_ << L" 倍";
        action_text_ = status.str();
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    POINT clamp_to_canvas(int x, int y) const {
        const RECT canvas = canvas_rect();
        return {std::clamp<LONG>(x, canvas.left, canvas.right - 1),
                std::clamp<LONG>(y, canvas.top, canvas.bottom - 1)};
    }

    void begin_box_zoom(int x, int y) {
        if (!view_valid_) return;
        box_start_ = clamp_to_canvas(x, y);
        box_end_ = box_start_;
        box_zooming_ = true;
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        action_text_ = L"拖动矩形选择要放大的地形范围，Esc 取消";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void update_box_zoom(int x, int y) {
        box_end_ = clamp_to_canvas(x, y);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void cancel_box_zoom() {
        box_zooming_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }

    void end_box_zoom(int x, int y) {
        box_end_ = clamp_to_canvas(x, y);
        const int width = std::abs(box_end_.x - box_start_.x);
        const int height = std::abs(box_end_.y - box_start_.y);
        cancel_box_zoom();
        if (width < 8 || height < 8) {
            action_text_ = L"框选区域过小，未执行放大";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        const auto first = screen_to_world(box_start_.x, box_start_.y);
        const auto second = screen_to_world(box_end_.x, box_end_.y);
        const dt_bounds2 selected{std::min(first.x, second.x),
                                  std::min(first.y, second.y),
                                  std::max(first.x, second.x),
                                  std::max(first.y, second.y)};
        fit_view_to_bounds(selected, 1.02);
        clear_overlays();
        action_text_ = L"框选范围已放大并适配当前窗口";
        invalidate_mesh_cache();
    }

    dt_point3 screen_to_world(int x, int y) const {
        const RECT canvas = canvas_rect();
        const double u = static_cast<double>(x - canvas.left) /
                         std::max(1L, canvas.right - canvas.left);
        const double v = static_cast<double>(y - canvas.top) /
                         std::max(1L, canvas.bottom - canvas.top);
        return {view_.xmin + u * (view_.xmax - view_.xmin),
                view_.ymax - v * (view_.ymax - view_.ymin), 0.0};
    }

    POINT world_to_screen(const dt_point3& point) const {
        const RECT canvas = canvas_rect();
        const double u = (point.x - view_.xmin) / (view_.xmax - view_.xmin);
        const double v = (view_.ymax - point.y) / (view_.ymax - view_.ymin);
        const double sx = canvas.left + u * (canvas.right - canvas.left);
        const double sy = canvas.top + v * (canvas.bottom - canvas.top);
        constexpr double limit = 1000000.0;
        return {static_cast<LONG>(std::clamp(sx, -limit, limit)),
                static_cast<LONG>(std::clamp(sy, -limit, limit))};
    }

    void ensure_cache() {
        if (cache_valid_ || !view_valid_) return;
        const auto begin = std::chrono::steady_clock::now();
        dt_statistics statistics{};
        if (!show_tin_ || dt_get_statistics(mesh_, &statistics) != DT_OK ||
            statistics.finite_triangle_count == 0) {
            triangles_.clear();
        } else {
            dt_query_result result = nullptr;
            if (dt_query_triangles(mesh_, &view_, &result) != DT_OK) {
                action_text_ = L"TIN 范围查询失败：" + last_error_text();
                return;
            }
            dt_query_result_view result_view{};
            if (dt_query_result_get_view(result, &result_view) == DT_OK) {
                if (result_view.triangle_count)
                    triangles_.assign(result_view.triangles,
                                      result_view.triangles + result_view.triangle_count);
                else
                    triangles_.clear();
            }
            dt_release_query_result(result);
        }

        cdt_triangles_.clear();
        if (show_cdt_ && cdt_) {
            dt_cdt_statistics cdt_stats{};
            cdt_stats.struct_size = sizeof(cdt_stats);
            if (dt_cdt_get_statistics(cdt_, &cdt_stats) == DT_OK &&
                cdt_stats.domain_triangle_count != 0) {
                cdt_info_ = cdt_stats;
                dt_cdt_query_result result = nullptr;
                if (dt_cdt_query_triangles(cdt_, &view_, &result) != DT_OK) {
                    action_text_ = L"CDT 范围查询失败：" + last_error_text();
                    return;
                }
                dt_cdt_query_result_view result_view{};
                if (dt_cdt_query_result_get_view(result, &result_view) == DT_OK &&
                    result_view.triangle_count) {
                    cdt_triangles_.assign(
                        result_view.triangles,
                        result_view.triangles + result_view.triangle_count);
                }
                dt_cdt_release_query_result(result);
            }
        }
        cache_valid_ = true;
        const auto end = std::chrono::steady_clock::now();
        last_query_ms_ = std::chrono::duration<double, std::milli>(end - begin).count();
    }

    bool is_grid_nodata(double value) const {
        return is_grid_nodata(grid_info_, value);
    }

    static bool is_grid_nodata(const dt_grid_info& info, double value) {
        if ((info.flags & DT_GRID_HAS_NODATA) == 0) return false;
        return std::isnan(info.nodata_value)
                   ? std::isnan(value)
                   : value == info.nodata_value;
    }

    static uint32_t grid_pixel(double ratio) {
        constexpr std::array<std::array<int, 3>, 8> colors{{
            {28, 70, 110}, {32, 108, 132}, {48, 139, 115}, {84, 156, 88},
            {135, 164, 76}, {180, 151, 84}, {204, 180, 125}, {232, 226, 199}}};
        ratio = std::clamp(ratio, 0.0, 1.0);
        const double scaled = ratio * static_cast<double>(colors.size() - 1);
        const size_t first = static_cast<size_t>(scaled);
        const size_t second = std::min(first + 1, colors.size() - 1);
        const double blend = scaled - static_cast<double>(first);
        auto channel = [&](size_t value) {
            return static_cast<uint32_t>(std::lround(
                colors[first][value] * (1.0 - blend) +
                colors[second][value] * blend));
        };
        const uint32_t red = channel(0);
        const uint32_t green = channel(1);
        const uint32_t blue = channel(2);
        return blue | (green << 8U) | (red << 16U);
    }

    static uint32_t slope_pixel(double slope_degrees) {
        constexpr std::array<std::array<int, 3>, 7> colors{{
            {34, 139, 84}, {112, 178, 88}, {214, 211, 92}, {239, 170, 70},
            {220, 83, 60}, {154, 49, 92}, {83, 32, 92}}};
        const double scaled = std::clamp(slope_degrees / 60.0, 0.0, 1.0) *
                              static_cast<double>(colors.size() - 1);
        const size_t first = static_cast<size_t>(scaled);
        const size_t second = std::min(first + 1, colors.size() - 1);
        const double blend = scaled - static_cast<double>(first);
        const auto channel = [&](size_t value) {
            return static_cast<uint32_t>(std::lround(
                colors[first][value] * (1.0 - blend) +
                colors[second][value] * blend));
        };
        const uint32_t red = channel(0), green = channel(1), blue = channel(2);
        return blue | (green << 8U) | (red << 16U);
    }

    static uint32_t aspect_pixel(double aspect_degrees) {
        double hue = std::fmod(aspect_degrees, 360.0);
        if (hue < 0.0) hue += 360.0;
        const double h = hue / 60.0;
        const int sector = static_cast<int>(std::floor(h)) % 6;
        const double fraction = h - std::floor(h);
        constexpr double saturation = 0.72;
        constexpr double brightness = 0.94;
        const double p = brightness * (1.0 - saturation);
        const double q = brightness * (1.0 - saturation * fraction);
        const double t = brightness * (1.0 - saturation * (1.0 - fraction));
        double red = 0.0, green = 0.0, blue = 0.0;
        switch (sector) {
        case 0: red = brightness; green = t; blue = p; break;
        case 1: red = q; green = brightness; blue = p; break;
        case 2: red = p; green = brightness; blue = t; break;
        case 3: red = p; green = q; blue = brightness; break;
        case 4: red = t; green = p; blue = brightness; break;
        default: red = brightness; green = p; blue = q; break;
        }
        const uint32_t r = static_cast<uint32_t>(std::lround(red * 255.0));
        const uint32_t g = static_cast<uint32_t>(std::lround(green * 255.0));
        const uint32_t b = static_cast<uint32_t>(std::lround(blue * 255.0));
        return b | (g << 8U) | (r << 16U);
    }

    double earthwork_difference_scale() const {
        if (!earthwork_result_valid_ ||
            !std::isfinite(earthwork_result_.minimum_difference) ||
            !std::isfinite(earthwork_result_.maximum_difference)) {
            return 1.0;
        }
        return std::max(
            1e-12, std::max(std::abs(earthwork_result_.minimum_difference),
                            std::abs(earthwork_result_.maximum_difference)));
    }

    uint32_t terrain_pixel(double value) const {
        if (grid_theme_ == GridTheme::Slope) return slope_pixel(value);
        if (grid_theme_ == GridTheme::Aspect) return aspect_pixel(value);
        if (grid_theme_ == GridTheme::Hillshade) {
            const uint32_t gray = static_cast<uint32_t>(std::lround(
                std::clamp(value, 0.0, 255.0)));
            return gray | (gray << 8U) | (gray << 16U);
        }
        if (grid_theme_ == GridTheme::Difference) {
            const double scale = earthwork_difference_scale();
            const double ratio =
                std::clamp(std::abs(value) / scale, 0.0, 1.0);
            const std::array<int, 3> neutral{245, 245, 245};
            const std::array<int, 3> end = value >= 0.0
                ? std::array<int, 3>{210, 45, 45}
                : std::array<int, 3>{49, 110, 189};
            const auto channel = [&](size_t index) {
                return static_cast<uint32_t>(std::lround(
                    neutral[index] * (1.0 - ratio) + end[index] * ratio));
            };
            const uint32_t red = channel(0);
            const uint32_t green = channel(1);
            const uint32_t blue = channel(2);
            return blue | (green << 8U) | (red << 16U);
        }
        return grid_pixel((value - grid_zmin_) / (grid_zmax_ - grid_zmin_));
    }

    static bool same_grid_preview_request(const GridPreviewRequest& request,
                                          dt_grid_handle source,
                                          const dt_grid_window& window,
                                          uint64_t output_width,
                                          uint64_t output_height,
                                          GridTheme theme) {
        return request.source == source && request.window.column == window.column &&
               request.window.row == window.row &&
               request.window.width == window.width &&
               request.window.height == window.height &&
               request.output_width == output_width &&
               request.output_height == output_height && request.theme == theme;
    }

    void destroy_grid_view_cache() {
        dt_grid_view_cache_destroy(grid_view_cache_);
        grid_view_cache_ = nullptr;
        grid_view_cache_source_ = nullptr;
    }

    bool ensure_grid_view_cache(dt_grid_handle source) {
        if (grid_view_cache_ && grid_view_cache_source_ == source) return true;
        destroy_grid_view_cache();
        dt_grid_view_cache_options options{};
        options.struct_size = sizeof(options);
        options.tile_width = 128;
        options.tile_height = 128;
        options.worker_count = terrain_worker_count_ == 0
            ? 0
            : std::min(terrain_worker_count_, 8U);
        options.maximum_bytes = 128ULL * 1024ULL * 1024ULL;
        options.maximum_tiles = 4096;
        dt_status status = DT_E_NOT_FOUND;
        if (source == grid_ && !grid_disk_cache_file_.empty()) {
            dt_grid_view_disk_cache_options disk{};
            disk.struct_size = sizeof(disk);
            disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_STALE |
                         DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
            disk.utf8_file_name = grid_disk_cache_file_.c_str();
            disk.source_revision = grid_disk_cache_revision_;
            disk.maximum_file_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
            status = dt_grid_view_cache_create_persistent(
                source, &options, &disk, &grid_view_cache_);
        }
        if (status != DT_OK) {
            status = dt_grid_view_cache_create(
                source, &options, &grid_view_cache_);
        }
        if (status != DT_OK) {
            grid_view_cache_ = nullptr;
            return false;
        }
        grid_view_cache_source_ = source;
        return true;
    }

    void retire_grid_preview_request() {
        if (!grid_preview_request_.task) return;
        dt_task_request_cancel(grid_preview_request_.task);
        retired_grid_preview_tasks_.push_back(grid_preview_request_.task);
        grid_preview_request_ = {};
    }

    void cancel_grid_preview_tasks(bool wait) {
        retire_grid_preview_request();
        for (dt_task_handle task : retired_grid_preview_tasks_)
            dt_task_request_cancel(task);
        failed_grid_preview_request_ = {};
        if (wait) {
            for (dt_task_handle task : retired_grid_preview_tasks_)
                dt_task_destroy(task);
            retired_grid_preview_tasks_.clear();
            if (hwnd_) KillTimer(hwnd_, kGridPreviewTimer);
        } else if (!retired_grid_preview_tasks_.empty() && hwnd_) {
            SetTimer(hwnd_, kGridPreviewTimer,
                     kGridPreviewPollMilliseconds, nullptr);
        }
    }

    void shutdown_grid_preview_tasks() {
        cancel_grid_preview_tasks(true);
    }

    void poll_grid_preview_tasks() {
        bool reaped_retired = false;
        auto retired = retired_grid_preview_tasks_.begin();
        while (retired != retired_grid_preview_tasks_.end()) {
            int32_t completed = 0;
            if (dt_task_wait(*retired, 0, &completed) != DT_OK || !completed) {
                ++retired;
                continue;
            }
            dt_task_destroy(*retired);
            retired = retired_grid_preview_tasks_.erase(retired);
            reaped_retired = true;
        }

        bool cache_updated = false;
        bool request_failed = false;
        if (grid_preview_request_.task) {
            auto publish_view = [&](const GridPreviewRequest& request,
                                    const dt_grid_view_result& view) {
                if (request.source != (terrain_grid_ ? terrain_grid_ : grid_) ||
                    request.theme != grid_theme_ ||
                    view.width != request.output_width ||
                    view.height != request.output_height ||
                    view.source_window.column != request.window.column ||
                    view.source_window.row != request.window.row ||
                    view.source_window.width != request.window.width ||
                    view.source_window.height != request.window.height) {
                    return false;
                }
                grid_preview_.resize(static_cast<size_t>(
                    view.width * view.height));
                for (uint64_t row = 0; row < view.height; ++row) {
                    for (uint64_t column = 0; column < view.width; ++column) {
                        const double value = view.values[static_cast<size_t>(
                            row * view.row_stride + column)];
                        grid_preview_[static_cast<size_t>(
                            row * view.width + column)] =
                            is_grid_nodata(request.source_info, value)
                                ? 0x00191f24U
                                : terrain_pixel(value);
                    }
                }
                grid_preview_width_ = static_cast<uint32_t>(view.width);
                grid_preview_height_ = static_cast<uint32_t>(view.height);
                grid_preview_column_ = request.window.column;
                grid_preview_row_ = request.window.row;
                grid_preview_source_width_ = request.window.width;
                grid_preview_source_height_ = request.window.height;
                grid_preview_source_ = request.source;
                grid_preview_view_valid_ = true;
                grid_preview_used_pyramid_ =
                    (view.overview.flags & DT_GRID_OVERVIEW_USED_PYRAMID) != 0;
                grid_preview_used_tile_cache_ =
                    (view.flags & DT_GRID_VIEW_RESULT_USED_TILE_CACHE) != 0;
                grid_preview_used_disk_cache_ =
                    (view.flags & DT_GRID_VIEW_RESULT_DISK_CACHE_HIT) != 0;
                grid_preview_lod_scale_ = view.lod_scale;
                grid_preview_tile_count_ = view.tile_count;
                grid_preview_reused_tile_count_ = view.reused_tile_count;
                grid_preview_theme_ = request.theme;
                failed_grid_preview_request_ = {};
                return true;
            };

            dt_grid_progressive_view_frame frame{};
            frame.struct_size = sizeof(frame);
            if (dt_task_get_grid_view_frame(
                    grid_preview_request_.task,
                    grid_preview_request_.last_frame_sequence,
                    &frame) == DT_OK) {
                grid_preview_request_.last_frame_sequence = frame.sequence;
                grid_preview_request_.final_frame_consumed =
                    (frame.flags & DT_GRID_PROGRESSIVE_FRAME_FINAL) != 0;
                cache_updated = publish_view(grid_preview_request_, frame.view) ||
                                cache_updated;
            }

            int32_t completed = 0;
            if (dt_task_wait(grid_preview_request_.task, 0, &completed) == DT_OK &&
                completed) {
                GridPreviewRequest request = grid_preview_request_;
                dt_task_info info{};
                info.struct_size = sizeof(info);
                dt_task_get_info(request.task, &info);
                if (info.state == DT_TASK_SUCCEEDED &&
                    request.last_frame_sequence == 0) {
                    dt_grid_view_result view{};
                    view.struct_size = sizeof(view);
                    if (dt_task_get_grid_view_result(request.task, &view) == DT_OK)
                        cache_updated = publish_view(request, view) ||
                                        cache_updated;
                    request.final_frame_consumed = true;
                } else if (info.state == DT_TASK_FAILED) {
                    char error[512]{};
                    if (dt_task_get_error(request.task, error, sizeof(error),
                                          nullptr) == DT_OK) {
                        action_text_ = L"异步 LOD 失败：" + utf8_to_wide(error);
                    }
                    failed_grid_preview_request_ = request;
                    failed_grid_preview_request_.task = nullptr;
                    request_failed = true;
                }
                if (info.state != DT_TASK_SUCCEEDED ||
                    grid_preview_request_.final_frame_consumed ||
                    request.final_frame_consumed) {
                    dt_task_destroy(request.task);
                    grid_preview_request_ = {};
                }
            }
        }

        if (!grid_preview_request_.task &&
            retired_grid_preview_tasks_.empty() && hwnd_) {
            KillTimer(hwnd_, kGridPreviewTimer);
        }
        if ((cache_updated || reaped_retired || request_failed) && hwnd_)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool refresh_grid_cache() {
        cancel_grid_preview_tasks(false);
        grid_preview_.clear();
        grid_preview_width_ = grid_preview_height_ = 0;
        grid_preview_used_pyramid_ = false;
        grid_preview_used_tile_cache_ = false;
        grid_preview_used_disk_cache_ = false;
        grid_preview_lod_scale_ = 1;
        grid_preview_tile_count_ = 0;
        grid_preview_reused_tile_count_ = 0;
        if (!grid_) return false;
        grid_info_ = {};
        grid_info_.struct_size = sizeof(grid_info_);
        if (dt_grid_get_info(grid_, &grid_info_) != DT_OK) return false;
        if (grid_info_.width == 0 || grid_info_.height == 0) return false;
        grid_preview_width_ = static_cast<uint32_t>(
            std::min<uint64_t>(512, grid_info_.width));
        grid_preview_height_ = static_cast<uint32_t>(
            std::min<uint64_t>(512, grid_info_.height));
        std::vector<double> overview(
            static_cast<size_t>(grid_preview_width_) * grid_preview_height_);
        dt_grid_overview_options options{};
        options.struct_size = sizeof(options);
        options.method = DT_GRID_OVERVIEW_AVERAGE;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        dt_grid_overview_result result{};
        if (dt_grid_read_overview(grid_, &options, grid_preview_width_,
                                  grid_preview_height_, overview.data(), 0,
                                  &result) != DT_OK) {
            grid_preview_width_ = grid_preview_height_ = 0;
            return false;
        }
        grid_preview_column_ = grid_preview_row_ = 0;
        grid_preview_source_width_ = grid_info_.width;
        grid_preview_source_height_ = grid_info_.height;
        grid_preview_source_ = grid_;
        grid_preview_view_valid_ = false;
        grid_preview_theme_ = grid_theme_;
        grid_preview_used_tile_cache_ = false;
        grid_preview_used_disk_cache_ = false;
        grid_preview_lod_scale_ = 1;
        grid_preview_tile_count_ = 0;
        grid_preview_reused_tile_count_ = 0;
        if (result.valid_value_count == 0) {
            grid_zmin_ = 0.0;
            grid_zmax_ = 1.0;
        } else {
            grid_zmin_ = result.minimum_value;
            grid_zmax_ = result.maximum_value;
        }
        if (grid_zmax_ == grid_zmin_) {
            grid_zmax_ = grid_zmin_ + 1.0;
        }
        grid_preview_.resize(static_cast<size_t>(grid_preview_width_) *
                             grid_preview_height_);
        for (uint32_t row = 0; row < grid_preview_height_; ++row) {
            for (uint32_t column = 0; column < grid_preview_width_; ++column) {
                const double value = overview[static_cast<size_t>(row) *
                                              grid_preview_width_ + column];
                grid_preview_[static_cast<size_t>(row) * grid_preview_width_ +
                              column] = is_grid_nodata(value)
                                            ? 0x00191f24U
                                            : grid_pixel((value - grid_zmin_) /
                                                         (grid_zmax_ - grid_zmin_));
            }
        }
        return true;
    }

    bool refresh_terrain_cache() {
        cancel_grid_preview_tasks(false);
        if (!terrain_grid_) return false;
        dt_grid_info info{};
        info.struct_size = sizeof(info);
        if (dt_grid_get_info(terrain_grid_, &info) != DT_OK) return false;
        grid_preview_.clear();
        grid_preview_width_ = grid_preview_height_ = 0;
        grid_preview_used_pyramid_ = false;
        grid_preview_used_tile_cache_ = false;
        grid_preview_used_disk_cache_ = false;
        grid_preview_lod_scale_ = 1;
        grid_preview_tile_count_ = 0;
        grid_preview_reused_tile_count_ = 0;
        if (info.width == 0 || info.height == 0) return false;
        grid_preview_width_ = static_cast<uint32_t>(std::min<uint64_t>(512, info.width));
        grid_preview_height_ = static_cast<uint32_t>(std::min<uint64_t>(512, info.height));
        std::vector<double> values(static_cast<size_t>(grid_preview_width_) *
                                   grid_preview_height_);
        dt_grid_overview_options options{};
        options.struct_size = sizeof(options);
        options.method = grid_theme_ == GridTheme::Aspect
            ? DT_GRID_OVERVIEW_NEAREST
            : DT_GRID_OVERVIEW_AVERAGE;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        if (dt_grid_read_overview(terrain_grid_, &options,
                                  grid_preview_width_, grid_preview_height_,
                                  values.data(), 0, nullptr) != DT_OK) {
            grid_preview_width_ = grid_preview_height_ = 0;
            return false;
        }
        grid_preview_column_ = grid_preview_row_ = 0;
        grid_preview_source_width_ = info.width;
        grid_preview_source_height_ = info.height;
        grid_preview_source_ = terrain_grid_;
        grid_preview_view_valid_ = false;
        grid_preview_theme_ = grid_theme_;
        grid_preview_used_tile_cache_ = false;
        grid_preview_used_disk_cache_ = false;
        grid_preview_lod_scale_ = 1;
        grid_preview_tile_count_ = 0;
        grid_preview_reused_tile_count_ = 0;
        grid_preview_.resize(static_cast<size_t>(grid_preview_width_) *
                             grid_preview_height_);
        for (uint32_t row = 0; row < grid_preview_height_; ++row) {
            for (uint32_t column = 0; column < grid_preview_width_; ++column) {
                const double value = values[static_cast<size_t>(
                    row) * grid_preview_width_ + column];
                grid_preview_[static_cast<size_t>(row) * grid_preview_width_ +
                              column] = is_grid_nodata(info, value)
                                            ? 0x00191f24U
                                            : terrain_pixel(value);
            }
        }
        return true;
    }

    bool refresh_grid_view_cache() {
        if (!view_valid_ || !grid_ || !show_grid_) {
            if (grid_preview_request_.task) retire_grid_preview_request();
            return true;
        }
        dt_grid_handle source_grid = terrain_grid_ ? terrain_grid_ : grid_;
        dt_grid_info source_info{};
        source_info.struct_size = sizeof(source_info);
        if (dt_grid_get_info(source_grid, &source_info) != DT_OK) return false;

        dt_grid_view_options view_options{};
        view_options.struct_size = sizeof(view_options);
        view_options.world_bounds = view_;
        view_options.padding_nodes = 2;
        dt_grid_window window{};
        const dt_status window_status = dt_grid_get_view_window(
            source_grid, &view_options, &window);
        if (window_status == DT_E_NOT_FOUND) {
            retire_grid_preview_request();
            grid_preview_.clear();
            grid_preview_width_ = grid_preview_height_ = 0;
            grid_preview_source_width_ = grid_preview_source_height_ = 0;
            grid_preview_source_ = source_grid;
            grid_preview_view_valid_ = true;
            grid_preview_used_pyramid_ = false;
            grid_preview_used_tile_cache_ = false;
            grid_preview_used_disk_cache_ = false;
            grid_preview_lod_scale_ = 1;
            grid_preview_tile_count_ = 0;
            grid_preview_reused_tile_count_ = 0;
            grid_preview_theme_ = grid_theme_;
            return true;
        }
        if (window_status != DT_OK) return false;

        const RECT canvas = canvas_rect();
        const uint64_t pixel_width = static_cast<uint64_t>(
            std::clamp<LONG>(canvas.right - canvas.left, 64, 512));
        const uint64_t pixel_height = static_cast<uint64_t>(
            std::clamp<LONG>(canvas.bottom - canvas.top, 64, 512));
        const uint64_t output_width = std::min(window.width, pixel_width);
        const uint64_t output_height = std::min(window.height, pixel_height);
        if (grid_preview_view_valid_ && grid_preview_source_ == source_grid &&
            grid_preview_column_ == window.column &&
            grid_preview_row_ == window.row &&
            grid_preview_source_width_ == window.width &&
            grid_preview_source_height_ == window.height &&
            grid_preview_width_ == output_width &&
            grid_preview_height_ == output_height &&
            grid_preview_theme_ == grid_theme_) {
            if (grid_preview_request_.task &&
                !same_grid_preview_request(grid_preview_request_, source_grid,
                                           window, output_width, output_height,
                                           grid_theme_)) {
                retire_grid_preview_request();
            }
            return true;
        }
        if (grid_preview_request_.task &&
            same_grid_preview_request(grid_preview_request_, source_grid,
                                      window, output_width, output_height,
                                      grid_theme_)) {
            return true;
        }
        if (same_grid_preview_request(failed_grid_preview_request_, source_grid,
                                      window, output_width, output_height,
                                      grid_theme_)) {
            return true;
        }

        failed_grid_preview_request_ = {};
        retire_grid_preview_request();
        if (retired_grid_preview_tasks_.size() >= 3) {
            SetTimer(hwnd_, kGridPreviewTimer,
                     kGridPreviewPollMilliseconds, nullptr);
            return true;
        }

        dt_grid_view_request_options request{};
        request.struct_size = sizeof(request);
        request.world_bounds = view_;
        request.output_width = output_width;
        request.output_height = output_height;
        request.padding_nodes = 2;
        request.overview_method = grid_theme_ == GridTheme::Aspect
            ? DT_GRID_OVERVIEW_NEAREST
            : DT_GRID_OVERVIEW_AVERAGE;
        request.worker_count = terrain_worker_count_;
        request.tile_row_count = terrain_tile_rows_;
        if (request.overview_method == DT_GRID_OVERVIEW_AVERAGE &&
            (source_info.flags & DT_GRID_HAS_PYRAMID) != 0) {
            request.flags |= DT_GRID_VIEW_REQUEST_USE_PYRAMID;
        } else {
            request.flags |= DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE;
        }
        if ((source_info.flags & DT_GRID_HAS_BLOCK_CHECKSUMS) != 0) {
            request.flags |= DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS;
        }
        if (!ensure_grid_view_cache(source_grid)) return false;
        dt_task_handle task = nullptr;
        dt_grid_progressive_view_options progressive{};
        progressive.struct_size = sizeof(progressive);
        progressive.initial_lod_multiplier = 4;
        progressive.maximum_frame_count = 3;
        if (dt_grid_read_view_progressive_async(
                grid_view_cache_, &request, &progressive, &task) != DT_OK) {
            return false;
        }
        grid_preview_request_.task = task;
        grid_preview_request_.source = source_grid;
        grid_preview_request_.source_info = source_info;
        grid_preview_request_.window = window;
        grid_preview_request_.output_width = output_width;
        grid_preview_request_.output_height = output_height;
        grid_preview_request_.theme = grid_theme_;
        SetTimer(hwnd_, kGridPreviewTimer,
                 kGridPreviewPollMilliseconds, nullptr);
        return true;
    }

    static COLORREF pixel_colorref(uint32_t pixel) {
        return RGB((pixel >> 16U) & 0xffU, (pixel >> 8U) & 0xffU,
                   pixel & 0xffU);
    }

    void draw_grid_legend(HDC dc) const {
        if (grid_theme_ == GridTheme::Elevation || !terrain_grid_) return;
        RECT canvas = canvas_rect();
        RECT panel{canvas.right - 190, canvas.top + 14,
                   canvas.right - 14, canvas.top + 74};
        HBRUSH background = CreateSolidBrush(RGB(248, 249, 250));
        FillRect(dc, &panel, background);
        DeleteObject(background);
        FrameRect(dc, &panel, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(30, 36, 42));
        RECT title{panel.left + 8, panel.top + 5, panel.right - 8, panel.top + 24};
        const wchar_t* label = grid_theme_ == GridTheme::Slope
                                   ? L"坡度（°）  0 — 60+"
                                   : grid_theme_ == GridTheme::Aspect
                                         ? L"坡向（°）  北→东→南→西"
                                         : grid_theme_ == GridTheme::Difference
                                               ? L"高差  蓝=填  白=0  红=挖"
                                               : L"阴影值  0 — 255";
        DrawTextW(dc, label, -1, &title, DT_LEFT | DT_SINGLELINE);
        const int left = panel.left + 8;
        const int top = panel.top + 31;
        const int width = panel.right - panel.left - 16;
        for (int i = 0; i < width; ++i) {
            const double ratio = static_cast<double>(i) /
                                 static_cast<double>(std::max(1, width - 1));
            uint32_t pixel = grid_theme_ == GridTheme::Slope
                                 ? slope_pixel(ratio * 60.0)
                                 : grid_theme_ == GridTheme::Aspect
                                       ? aspect_pixel(ratio * 360.0)
                                       : grid_theme_ == GridTheme::Difference
                                             ? terrain_pixel((ratio * 2.0 - 1.0) *
                                                   earthwork_difference_scale())
                                             : static_cast<uint32_t>(ratio * 255.0) *
                                                   0x00010101U;
            HPEN pen = CreatePen(PS_SOLID, 1, pixel_colorref(pixel));
            const auto old = SelectObject(dc, pen);
            MoveToEx(dc, left + i, top, nullptr);
            LineTo(dc, left + i, top + 13);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
    }

    dt_point3 grid_world(uint64_t column, uint64_t row) const {
        const double c = static_cast<double>(column);
        const double r = static_cast<double>(row);
        return {grid_info_.geo_transform[0] + c * grid_info_.geo_transform[1] +
                    r * grid_info_.geo_transform[2],
                grid_info_.geo_transform[3] + c * grid_info_.geo_transform[4] +
                    r * grid_info_.geo_transform[5],
                0.0};
    }

    void draw_grid(HDC dc) const {
        if (!grid_ || !show_grid_ || grid_info_.width == 0 ||
            grid_info_.height == 0) return;
        const POINT corners[4]{world_to_screen(grid_world(0, 0)),
            world_to_screen(grid_world(grid_info_.width - 1, 0)),
            world_to_screen(grid_world(grid_info_.width - 1,
                                       grid_info_.height - 1)),
            world_to_screen(grid_world(0, grid_info_.height - 1))};
        if (!grid_preview_.empty() && grid_preview_source_width_ != 0 &&
            grid_preview_source_height_ != 0) {
            const uint64_t last_column = grid_preview_column_ +
                                         grid_preview_source_width_ - 1;
            const uint64_t last_row = grid_preview_row_ +
                                      grid_preview_source_height_ - 1;
            const POINT preview_corners[4]{
                world_to_screen(grid_world(grid_preview_column_,
                                           grid_preview_row_)),
                world_to_screen(grid_world(last_column, grid_preview_row_)),
                world_to_screen(grid_world(last_column, last_row)),
                world_to_screen(grid_world(grid_preview_column_, last_row))};
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(grid_preview_width_);
            info.bmiHeader.biHeight = -static_cast<LONG>(grid_preview_height_);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                              &pixels, nullptr, 0);
            if (bitmap && pixels) {
                std::memcpy(pixels, grid_preview_.data(),
                            grid_preview_.size() * sizeof(uint32_t));
                HDC source = CreateCompatibleDC(dc);
                const auto old_bitmap = SelectObject(source, bitmap);
                POINT destination[3]{preview_corners[0], preview_corners[1],
                                     preview_corners[3]};
                SetStretchBltMode(dc, HALFTONE);
                PlgBlt(dc, destination, source, 0, 0,
                       static_cast<int>(grid_preview_width_),
                       static_cast<int>(grid_preview_height_), nullptr, 0, 0);
                SelectObject(source, old_bitmap);
                DeleteDC(source);
            }
            if (bitmap) DeleteObject(bitmap);
        }
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(81, 184, 232));
        const auto old_pen = SelectObject(dc, pen);
        POINT outline[5]{corners[0], corners[1], corners[2], corners[3], corners[0]};
        Polyline(dc, outline, 5);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        draw_grid_legend(dc);
    }

    void draw_contours(HDC dc) const {
        if (!contours_ || !show_contours_ || contour_info_.line_count == 0) return;
        const uint64_t point_step = std::max<uint64_t>(
            1, (contour_info_.vertex_count + kMaxContourDrawVertices - 1) /
                   kMaxContourDrawVertices);
        const uint64_t line_step = std::max<uint64_t>(
            1, (contour_info_.line_count + 19999) / 20000);
        HPEN minor_pen = CreatePen(PS_SOLID, 1, RGB(255, 202, 73));
        HPEN major_pen = CreatePen(PS_SOLID, 2, RGB(255, 242, 184));
        for (uint64_t index = 0; index < contour_info_.line_count;
             index += line_step) {
            dt_contour_line_view line{};
            line.struct_size = sizeof(line);
            if (dt_contours_get_line(contours_, index, &line) != DT_OK ||
                line.point_count < 2) continue;
            std::vector<POINT> points;
            points.reserve(static_cast<size_t>(line.point_count / point_step + 2));
            for (uint64_t point = 0; point < line.point_count; point += point_step)
                points.push_back(world_to_screen(line.points[point]));
            if ((line.point_count - 1) % point_step != 0)
                points.push_back(world_to_screen(line.points[line.point_count - 1]));
            if ((line.flags & DT_CONTOUR_LINE_CLOSED) != 0 &&
                (points.front().x != points.back().x ||
                 points.front().y != points.back().y))
                points.push_back(points.front());
            const auto old_pen = SelectObject(
                dc, (index / line_step) % 5 == 0 ? major_pen : minor_pen);
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, old_pen);
        }
        DeleteObject(minor_pen);
        DeleteObject(major_pen);
    }

    void draw_cdt(HDC dc) const {
        if (!show_cdt_) return;
        if (!cdt_triangles_.empty()) {
            const size_t step = std::max<size_t>(
                1, (cdt_triangles_.size() + kMaxDrawTriangles - 1) /
                       kMaxDrawTriangles);
            HPEN mesh_pen = CreatePen(PS_SOLID, 1, RGB(151, 126, 205));
            const auto old_pen = SelectObject(dc, mesh_pen);
            for (size_t i = 0; i < cdt_triangles_.size(); i += step) {
                const auto& triangle = cdt_triangles_[i];
                POINT points[4]{world_to_screen(triangle.vertex[0].point),
                                world_to_screen(triangle.vertex[1].point),
                                world_to_screen(triangle.vertex[2].point),
                                world_to_screen(triangle.vertex[0].point)};
                Polyline(dc, points, 4);
            }
            SelectObject(dc, old_pen);
            DeleteObject(mesh_pen);
        }
        HPEN breakline_pen = CreatePen(PS_SOLID, 3, RGB(255, 145, 55));
        HPEN outer_pen = CreatePen(PS_SOLID, 4, RGB(55, 225, 238));
        HPEN hole_pen = CreatePen(PS_SOLID, 4, RGB(255, 92, 178));
        for (const auto& line : constraint_lines_) {
            if (line.points.size() < 2) continue;
            std::vector<POINT> points;
            points.reserve(line.points.size() + 1);
            for (const auto& point : line.points)
                points.push_back(world_to_screen(point));
            if ((line.flags & DT_CONSTRAINT_CLOSED) != 0)
                points.push_back(points.front());
            HPEN pen = line.kind == DT_CONSTRAINT_OUTER_BOUNDARY
                           ? outer_pen
                           : line.kind == DT_CONSTRAINT_HOLE_BOUNDARY
                                 ? hole_pen
                                 : breakline_pen;
            const auto old_pen = SelectObject(dc, pen);
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, old_pen);
        }
        if (mode_ == Mode::CdtMoveVertex ||
            mode_ == Mode::CdtRemoveVertex) {
            HPEN point_pen = CreatePen(PS_SOLID, 1, RGB(28, 32, 38));
            HBRUSH point_brush = CreateSolidBrush(RGB(245, 245, 245));
            HBRUSH selected_brush = CreateSolidBrush(RGB(255, 238, 70));
            const auto old_pen = SelectObject(dc, point_pen);
            const auto old_brush = SelectObject(dc, point_brush);
            for (const auto& line : constraint_lines_) {
                for (size_t index = 0; index < line.points.size(); ++index) {
                    const POINT point = world_to_screen(line.points[index]);
                    const bool selected =
                        line.id == cdt_move_constraint_id_ &&
                        index == cdt_move_point_index_;
                    SelectObject(dc, selected ? selected_brush : point_brush);
                    const int radius = selected ? 7 : 3;
                    Ellipse(dc, point.x - radius, point.y - radius,
                            point.x + radius + 1, point.y + radius + 1);
                }
            }
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(selected_brush);
            DeleteObject(point_brush);
            DeleteObject(point_pen);
        }
        if (!cdt_draft_.empty()) {
            std::vector<POINT> points;
            points.reserve(cdt_draft_.size() + 1);
            for (const auto& point : cdt_draft_)
                points.push_back(world_to_screen(point));
            if (cdt_draft_kind_ != DT_CONSTRAINT_BREAKLINE &&
                points.size() >= 2)
                points.push_back(points.front());
            HPEN draft_pen = CreatePen(PS_SOLID, 2, RGB(255, 238, 70));
            const auto old_pen = SelectObject(dc, draft_pen);
            if (points.size() >= 2)
                Polyline(dc, points.data(), static_cast<int>(points.size()));
            const POINT last = world_to_screen(cdt_draft_.back());
            HBRUSH brush = CreateSolidBrush(RGB(255, 238, 70));
            const auto old_brush = SelectObject(dc, brush);
            Ellipse(dc, last.x - 4, last.y - 4, last.x + 5, last.y + 5);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(brush);
            DeleteObject(draft_pen);
        }
        DeleteObject(breakline_pen);
        DeleteObject(outer_pen);
        DeleteObject(hole_pen);
    }

    void update_model_metrics() {
        if (triangles_.empty()) {
            model_center_x_ = model_center_y_ = model_center_z_ = 0.0;
            model_xy_scale_ = 1.0;
            mesh_zmin_ = 0.0;
            mesh_zmax_ = 1.0;
            return;
        }
        double xmin = std::numeric_limits<double>::max();
        double ymin = xmin;
        double zmin = xmin;
        double xmax = std::numeric_limits<double>::lowest();
        double ymax = xmax;
        double zmax = xmax;
        for (const auto& triangle : triangles_) {
            for (const auto& vertex : triangle.vertex) {
                xmin = std::min(xmin, vertex.point.x);
                ymin = std::min(ymin, vertex.point.y);
                zmin = std::min(zmin, vertex.point.z);
                xmax = std::max(xmax, vertex.point.x);
                ymax = std::max(ymax, vertex.point.y);
                zmax = std::max(zmax, vertex.point.z);
            }
        }
        model_center_x_ = (xmin + xmax) * 0.5;
        model_center_y_ = (ymin + ymax) * 0.5;
        model_center_z_ = (zmin + zmax) * 0.5;
        model_xy_scale_ = std::max({(xmax - xmin) * 0.5,
                                    (ymax - ymin) * 0.5, 1.0e-12});
        mesh_zmin_ = zmin;
        mesh_zmax_ = zmax > zmin ? zmax : zmin + 1.0;
    }

    void reset_camera3d() {
        update_model_metrics();
        camera_ = dterrain::viewer3d::Camera{};
        camera_.distance = 3.2;
        camera_needs_reset_ = false;
    }

    dterrain::viewer3d::Vec3 normalized_point(const dt_point3& point) const {
        return {(point.x - model_center_x_) / model_xy_scale_,
                (point.y - model_center_y_) / model_xy_scale_,
                (point.z - model_center_z_) / model_xy_scale_ * z_exaggeration_};
    }

    struct ProjectedTriangle3d {
        POINT point[3]{};
        double depth = 0.0;
        size_t color_band = 0;
    };

    bool project_to_canvas(const dterrain::viewer3d::Vec3& point,
                           const RECT& canvas, POINT& screen,
                           double& depth) const {
        const double width = static_cast<double>(std::max(1L, canvas.right - canvas.left));
        const double height = static_cast<double>(std::max(1L, canvas.bottom - canvas.top));
        const auto projected = dterrain::viewer3d::project(
            point, camera_, width / height);
        if (!projected.visible || std::abs(projected.x) > 3.0 ||
            std::abs(projected.y) > 3.0) return false;
        const double sx = canvas.left + (projected.x + 1.0) * 0.5 * width;
        const double sy = canvas.top + (1.0 - projected.y) * 0.5 * height;
        constexpr double limit = 1000000.0;
        screen = {static_cast<LONG>(std::clamp(sx, -limit, limit)),
                  static_cast<LONG>(std::clamp(sy, -limit, limit))};
        depth = projected.depth;
        return true;
    }

    void draw_axes3d(HDC dc, const RECT& canvas) const {
        const std::array<dterrain::viewer3d::Vec3, 4> axis_points{
            dterrain::viewer3d::Vec3{-1.0, -1.0, -0.02},
            dterrain::viewer3d::Vec3{-0.62, -1.0, -0.02},
            dterrain::viewer3d::Vec3{-1.0, -0.62, -0.02},
            dterrain::viewer3d::Vec3{-1.0, -1.0, 0.36}};
        POINT screen[4]{};
        double depth = 0.0;
        for (size_t i = 0; i < axis_points.size(); ++i)
            if (!project_to_canvas(axis_points[i], canvas, screen[i], depth)) return;
        const std::array<COLORREF, 3> colors{
            RGB(244, 94, 94), RGB(93, 220, 126), RGB(88, 160, 255)};
        for (size_t i = 0; i < colors.size(); ++i) {
            HPEN pen = CreatePen(PS_SOLID, 3, colors[i]);
            const auto old_pen = SelectObject(dc, pen);
            POINT line[2]{screen[0], screen[i + 1]};
            Polyline(dc, line, 2);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
    }

    void draw_mesh3d(HDC dc, const RECT& canvas) {
        if (triangles_.empty()) return;
        if (camera_needs_reset_) reset_camera3d();
        const size_t step = std::max<size_t>(1,
            (triangles_.size() + kMaxDrawTriangles3d - 1) /
            kMaxDrawTriangles3d);
        std::vector<ProjectedTriangle3d> projected;
        projected.reserve((triangles_.size() + step - 1) / step);
        const auto light = dterrain::viewer3d::normalized(
            dterrain::viewer3d::Vec3{-0.35, -0.45, 0.82});
        for (size_t i = 0; i < triangles_.size(); i += step) {
            const auto& triangle = triangles_[i];
            const auto a = normalized_point(triangle.vertex[0].point);
            const auto b = normalized_point(triangle.vertex[1].point);
            const auto c = normalized_point(triangle.vertex[2].point);
            ProjectedTriangle3d item{};
            double depth[3]{};
            if (!project_to_canvas(a, canvas, item.point[0], depth[0]) ||
                !project_to_canvas(b, canvas, item.point[1], depth[1]) ||
                !project_to_canvas(c, canvas, item.point[2], depth[2])) continue;
            item.depth = (depth[0] + depth[1] + depth[2]) / 3.0;
            const double z = (triangle.vertex[0].point.z +
                              triangle.vertex[1].point.z +
                              triangle.vertex[2].point.z) / 3.0;
            const double z_ratio = std::clamp((z - mesh_zmin_) /
                                              (mesh_zmax_ - mesh_zmin_), 0.0, 1.0);
            const auto normal = dterrain::viewer3d::normalized(
                dterrain::viewer3d::cross(b - a, c - a));
            const double lighting = 0.58 + 0.42 * std::abs(
                dterrain::viewer3d::dot(normal, light));
            item.color_band = std::min<size_t>(9, static_cast<size_t>(
                std::clamp(z_ratio * 8.0 + (lighting - 0.58) * 3.0,
                           0.0, 9.0)));
            projected.push_back(item);
        }
        std::sort(projected.begin(), projected.end(),
                  [](const auto& left, const auto& right) {
                      return left.depth > right.depth;
                  });
        constexpr std::array<COLORREF, 10> palette{
            RGB(31, 72, 112), RGB(35, 94, 125), RGB(39, 116, 126),
            RGB(50, 135, 111), RGB(76, 150, 91), RGB(111, 158, 78),
            RGB(149, 157, 77), RGB(178, 151, 94), RGB(198, 176, 128),
            RGB(225, 220, 190)};
        std::array<HBRUSH, palette.size()> brushes{};
        for (size_t i = 0; i < palette.size(); ++i)
            brushes[i] = CreateSolidBrush(palette[i]);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(34, 43, 47));
        const auto old_pen = SelectObject(dc, pen);
        for (const auto& triangle : projected) {
            const auto old_brush = SelectObject(dc, brushes[triangle.color_band]);
            Polygon(dc, triangle.point, 3);
            SelectObject(dc, old_brush);
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        for (HBRUSH brush : brushes) DeleteObject(brush);
        draw_axes3d(dc, canvas);
    }

    void draw_triangle_set(HDC dc, const std::vector<dt_triangle3>& source,
                           COLORREF color, int width) const {
        if (source.empty()) return;
        HPEN pen = CreatePen(PS_SOLID, width, color);
        const auto old_pen = SelectObject(dc, pen);
        for (const auto& triangle : source) {
            POINT points[4] = {world_to_screen(triangle.vertex[0].point),
                               world_to_screen(triangle.vertex[1].point),
                               world_to_screen(triangle.vertex[2].point),
                               world_to_screen(triangle.vertex[0].point)};
            Polyline(dc, points, 4);
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_segment_set(HDC dc, const std::vector<dt_segment3>& source,
                          COLORREF color, int width) const {
        if (source.empty()) return;
        HPEN pen = CreatePen(PS_SOLID, width, color);
        const auto old_pen = SelectObject(dc, pen);
        for (const auto& segment : source) {
            POINT points[2] = {world_to_screen(segment.vertex[0].point),
                               world_to_screen(segment.vertex[1].point)};
            Polyline(dc, points, 2);
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_mesh(HDC dc) const {
        if (triangles_.empty()) return;
        const size_t step = std::max<size_t>(1,
            (triangles_.size() + kMaxDrawTriangles - 1) / kMaxDrawTriangles);
        double zmin = std::numeric_limits<double>::max();
        double zmax = std::numeric_limits<double>::lowest();
        for (size_t i = 0; i < triangles_.size(); i += step) {
            for (const auto& vertex : triangles_[i].vertex) {
                zmin = std::min(zmin, vertex.point.z);
                zmax = std::max(zmax, vertex.point.z);
            }
        }
        if (zmax <= zmin) zmax = zmin + 1.0;

        constexpr std::array<COLORREF, 8> palette = {
            RGB(38, 85, 120), RGB(43, 112, 130), RGB(55, 135, 112),
            RGB(92, 151, 89), RGB(142, 158, 78), RGB(178, 144, 82),
            RGB(190, 170, 126), RGB(222, 224, 218)};
        std::array<std::vector<POINT>, 8> point_groups;
        std::array<std::vector<DWORD>, 8> count_groups;
        for (size_t i = 0; i < triangles_.size(); i += step) {
            const auto& triangle = triangles_[i];
            const double average = (triangle.vertex[0].point.z +
                                    triangle.vertex[1].point.z +
                                    triangle.vertex[2].point.z) / 3.0;
            const size_t band = std::min<size_t>(7, static_cast<size_t>(
                std::max(0.0, (average - zmin) / (zmax - zmin) * 8.0)));
            auto& points = point_groups[band];
            points.push_back(world_to_screen(triangle.vertex[0].point));
            points.push_back(world_to_screen(triangle.vertex[1].point));
            points.push_back(world_to_screen(triangle.vertex[2].point));
            points.push_back(world_to_screen(triangle.vertex[0].point));
            count_groups[band].push_back(4);
        }
        for (size_t band = 0; band < palette.size(); ++band) {
            if (count_groups[band].empty()) continue;
            HPEN pen = CreatePen(PS_SOLID, 1, palette[band]);
            const auto old_pen = SelectObject(dc, pen);
            PolyPolyline(dc, point_groups[band].data(), count_groups[band].data(),
                         static_cast<DWORD>(count_groups[band].size()));
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
    }

    void draw_highlight(HDC dc) const {
        if (has_highlight_triangle_) {
            std::vector<dt_triangle3> one{highlight_triangle_};
            draw_triangle_set(dc, one, RGB(255, 80, 220), 3);
        }
        if (has_highlight_vertex_) {
            const POINT point = world_to_screen(highlight_vertex_.point);
            HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
            const auto old_brush = SelectObject(dc, brush);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 70, 200));
            const auto old_pen = SelectObject(dc, pen);
            Ellipse(dc, point.x - 5, point.y - 5, point.x + 6, point.y + 6);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(pen);
            DeleteObject(brush);
        }
    }

    void draw_box_zoom(HDC dc) const {
        if (!box_zooming_) return;
        RECT box{std::min(box_start_.x, box_end_.x),
                 std::min(box_start_.y, box_end_.y),
                 std::max(box_start_.x, box_end_.x),
                 std::max(box_start_.y, box_end_.y)};
        HPEN pen = CreatePen(PS_DASH, 2, RGB(40, 220, 255));
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, box.left, box.top, box.right, box.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_profile(HDC dc, const RECT& canvas) const {
        if (!profile_has_start_) return;
        const POINT start = world_to_screen(profile_start_);
        HPEN line_pen = CreatePen(PS_SOLID, 3, RGB(70, 235, 255));
        const auto old_pen = SelectObject(dc, line_pen);
        if (profile_complete_) {
            const POINT end = world_to_screen(profile_end_);
            MoveToEx(dc, start.x, start.y, nullptr);
            LineTo(dc, end.x, end.y);
        }
        HBRUSH start_brush = CreateSolidBrush(RGB(60, 235, 120));
        const auto old_brush = SelectObject(dc, start_brush);
        Ellipse(dc, start.x - 6, start.y - 6, start.x + 7, start.y + 7);
        SelectObject(dc, old_brush);
        DeleteObject(start_brush);
        if (profile_complete_) {
            const POINT end = world_to_screen(profile_end_);
            HBRUSH end_brush = CreateSolidBrush(RGB(255, 105, 80));
            const auto previous_brush = SelectObject(dc, end_brush);
            Ellipse(dc, end.x - 6, end.y - 6, end.x + 7, end.y + 7);
            SelectObject(dc, previous_brush);
            DeleteObject(end_brush);
        }
        SelectObject(dc, old_pen);
        DeleteObject(line_pen);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 245, 250));
        TextOutW(dc, start.x + 8, start.y - 18, L"A", 1);
        if (!profile_complete_ || profile_samples_.empty()) return;
        const POINT end = world_to_screen(profile_end_);
        TextOutW(dc, end.x + 8, end.y - 18, L"B", 1);

        RECT panel = canvas;
        panel.left += 16;
        panel.right -= 16;
        panel.bottom -= 16;
        panel.top = std::max<LONG>(canvas.top + 16, panel.bottom - 210);
        HBRUSH panel_brush = CreateSolidBrush(RGB(24, 31, 39));
        FillRect(dc, &panel, panel_brush);
        DeleteObject(panel_brush);
        HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(80, 155, 180));
        const auto previous_pen = SelectObject(dc, border_pen);
        const auto hollow_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
        SelectObject(dc, hollow_brush);

        RECT plot{panel.left + 62, panel.top + 48,
                  panel.right - 24, panel.bottom - 30};
        const double raw_minimum = profile_statistics_.minimum_z;
        const double raw_maximum = profile_statistics_.maximum_z;
        const double range = std::max(1.0e-12, raw_maximum - raw_minimum);
        const double minimum = raw_minimum - range * 0.06;
        const double maximum = raw_maximum + range * 0.06;
        const double distance = std::max(
            1.0e-12, profile_statistics_.horizontal_distance);

        HPEN grid_pen = CreatePen(PS_DOT, 1, RGB(72, 82, 92));
        SelectObject(dc, grid_pen);
        SetTextColor(dc, RGB(190, 202, 212));
        for (int index = 0; index <= 4; ++index) {
            const LONG y = plot.bottom -
                (plot.bottom - plot.top) * index / 4;
            MoveToEx(dc, plot.left, y, nullptr);
            LineTo(dc, plot.right, y);
            std::wostringstream label;
            label << std::fixed << std::setprecision(1)
                  << (minimum + (maximum - minimum) * index / 4.0);
            RECT label_rect{panel.left + 4, y - 9, plot.left - 5, y + 10};
            DrawTextW(dc, label.str().c_str(), -1, &label_rect,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, border_pen);
        DeleteObject(grid_pen);

        HPEN profile_pen = CreatePen(PS_SOLID, 2, RGB(255, 210, 70));
        SelectObject(dc, profile_pen);
        bool previous_valid = false;
        for (const auto& sample : profile_samples_) {
            if (!sample.valid || !std::isfinite(sample.z)) {
                previous_valid = false;
                continue;
            }
            const LONG x = plot.left + static_cast<LONG>(std::lround(
                sample.distance / distance * (plot.right - plot.left)));
            const LONG y = plot.bottom - static_cast<LONG>(std::lround(
                (sample.z - minimum) / (maximum - minimum) *
                (plot.bottom - plot.top)));
            if (previous_valid) LineTo(dc, x, y);
            else MoveToEx(dc, x, y, nullptr);
            previous_valid = true;
        }
        SelectObject(dc, border_pen);
        DeleteObject(profile_pen);
        SelectObject(dc, previous_pen);
        DeleteObject(border_pen);

        SetTextColor(dc, RGB(232, 239, 244));
        std::wostringstream title;
        title << L"剖面 A→B｜" << profile_source_name() << L"｜距离 "
              << std::fixed << std::setprecision(2)
              << profile_statistics_.horizontal_distance << L"｜高程 "
              << raw_minimum << L"～" << raw_maximum;
        if (std::isfinite(profile_statistics_.elevation_difference))
            title << L"｜高差 " << profile_statistics_.elevation_difference;
        title << L"｜累计升/降 " << profile_statistics_.ascent << L"/"
              << profile_statistics_.descent << L"｜最大坡度 "
              << std::setprecision(1)
              << profile_statistics_.maximum_absolute_grade_percent << L"%";
        RECT title_rect{panel.left + 12, panel.top + 10,
                        panel.right - 12, panel.top + 38};
        DrawTextW(dc, title.str().c_str(), -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        TextOutW(dc, plot.left, plot.bottom + 6, L"0", 1);
        std::wostringstream distance_label;
        distance_label << std::fixed << std::setprecision(2) << distance;
        RECT end_label{plot.right - 150, plot.bottom + 4,
                       plot.right, panel.bottom - 4};
        DrawTextW(dc, distance_label.str().c_str(), -1, &end_label,
                  DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    void draw_measurement(HDC dc, const RECT& canvas) const {
        if (measurement_polygon_.empty()) return;
        std::vector<POINT> points;
        points.reserve(measurement_polygon_.size());
        for (const auto& point : measurement_polygon_)
            points.push_back(world_to_screen(point));
        SetBkMode(dc, TRANSPARENT);
        HPEN outline = CreatePen(
            PS_SOLID, measurement_complete_ ? 3 : 2,
            measurement_complete_ ? RGB(255, 150, 45) : RGB(255, 220, 70));
        const auto old_pen = SelectObject(dc, outline);
        if (measurement_complete_ && points.size() >= 3) {
            HBRUSH hatch = CreateHatchBrush(HS_DIAGCROSS, RGB(235, 130, 40));
            const auto old_brush = SelectObject(dc, hatch);
            Polygon(dc, points.data(), static_cast<int>(points.size()));
            SelectObject(dc, old_brush);
            DeleteObject(hatch);
        } else if (points.size() >= 2) {
            Polyline(dc, points.data(), static_cast<int>(points.size()));
        }
        SelectObject(dc, old_pen);
        DeleteObject(outline);

        HBRUSH vertex_brush = CreateSolidBrush(RGB(255, 235, 120));
        const auto old_brush = SelectObject(dc, vertex_brush);
        for (std::size_t index = 0; index < points.size(); ++index) {
            const int radius = index == 0 ? 6 : 4;
            Ellipse(dc, points[index].x - radius, points[index].y - radius,
                    points[index].x + radius + 1,
                    points[index].y + radius + 1);
        }
        SelectObject(dc, old_brush);
        DeleteObject(vertex_brush);
        SetTextColor(dc, RGB(255, 245, 200));
        TextOutW(dc, points.front().x + 8, points.front().y - 18, L"P1", 2);

        if (!measurement_complete_) return;
        const double coverage = measurement_statistics_.valid_plan_area /
                                measurement_statistics_.polygon.area * 100.0;
        RECT panel{canvas.left + 16, canvas.top + 16,
                   canvas.right - 16, canvas.top + 92};
        HBRUSH panel_brush = CreateSolidBrush(RGB(37, 31, 26));
        FillRect(dc, &panel, panel_brush);
        DeleteObject(panel_brush);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(230, 145, 55));
        const auto previous_pen = SelectObject(dc, border);
        const auto hollow = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
        SelectObject(dc, hollow);
        SelectObject(dc, previous_pen);
        DeleteObject(border);
        std::wostringstream text;
        text << L"面积/土方量测｜" << surface_source_name(measurement_source_)
             << L"｜投影面积 " << std::fixed << std::setprecision(2)
             << measurement_statistics_.polygon.area << L"｜周长 "
             << measurement_statistics_.polygon.perimeter << L"｜有效覆盖 "
             << coverage << L"%｜地表面积 "
             << measurement_statistics_.surface_area << L"\n平均/最低/最高 Z "
             << measurement_statistics_.mean_z << L" / "
             << measurement_statistics_.minimum_z << L" / "
             << measurement_statistics_.maximum_z << L"｜基准 Z "
             << measurement_datum_z_ << L"｜挖/填/净挖方 "
             << measurement_statistics_.cut_volume << L" / "
             << measurement_statistics_.fill_volume << L" / "
             << measurement_statistics_.net_cut_volume << L"（积分估算）";
        SetTextColor(dc, RGB(250, 235, 215));
        RECT text_rect{panel.left + 10, panel.top + 8,
                       panel.right - 10, panel.bottom - 6};
        DrawTextW(dc, text.str().c_str(), -1, &text_rect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    void draw_slope_analysis(HDC dc, const RECT& canvas) const {
        if (!slope_valid_) return;
        SetBkMode(dc, TRANSPARENT);
        std::vector<POINT> support;
        if (slope_analysis_.support_point_count == 3) {
            for (uint32_t index = 0; index < 3; ++index)
                support.push_back(world_to_screen(
                    slope_analysis_.support_points[index]));
        } else if (slope_analysis_.support_point_count == 4) {
            constexpr uint32_t order[4] = {0, 1, 3, 2};
            for (const uint32_t index : order)
                support.push_back(world_to_screen(
                    slope_analysis_.support_points[index]));
        }
        if (support.size() >= 3) {
            HPEN support_pen = CreatePen(PS_DOT, 1, RGB(100, 220, 220));
            const auto previous_pen = SelectObject(dc, support_pen);
            const auto previous_brush =
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Polygon(dc, support.data(), static_cast<int>(support.size()));
            SelectObject(dc, previous_brush);
            SelectObject(dc, previous_pen);
            DeleteObject(support_pen);
        }

        const POINT origin = world_to_screen(slope_analysis_.point);
        HPEN arrow_pen = CreatePen(PS_SOLID, 3, RGB(70, 235, 210));
        const auto previous_pen = SelectObject(dc, arrow_pen);
        HBRUSH marker = CreateSolidBrush(RGB(255, 245, 150));
        const auto previous_brush = SelectObject(dc, marker);
        Ellipse(dc, origin.x - 6, origin.y - 6,
                origin.x + 7, origin.y + 7);
        SelectObject(dc, previous_brush);
        DeleteObject(marker);
        if ((slope_analysis_.flags & DT_SURFACE_ASPECT_UNDEFINED) == 0) {
            constexpr double degrees_to_radians =
                3.141592653589793238462643383279502884 / 180.0;
            const double radians =
                slope_analysis_.aspect_degrees * degrees_to_radians;
            POINT end{origin.x + static_cast<LONG>(std::lround(
                          std::sin(radians) * 64.0)),
                      origin.y - static_cast<LONG>(std::lround(
                          std::cos(radians) * 64.0))};
            MoveToEx(dc, origin.x, origin.y, nullptr);
            LineTo(dc, end.x, end.y);
            const double screen_angle = std::atan2(
                static_cast<double>(end.y - origin.y),
                static_cast<double>(end.x - origin.x));
            for (const double offset : {2.55, -2.55}) {
                MoveToEx(dc, end.x, end.y, nullptr);
                LineTo(dc,
                       end.x + static_cast<LONG>(std::lround(
                           std::cos(screen_angle + offset) * 14.0)),
                       end.y + static_cast<LONG>(std::lround(
                           std::sin(screen_angle + offset) * 14.0)));
            }
        }
        SelectObject(dc, previous_pen);
        DeleteObject(arrow_pen);

        const LONG panel_top = canvas.top +
            (measurement_complete_ ? 108 : 16);
        RECT panel{std::max<LONG>(canvas.left + 16, canvas.right - 520),
                   panel_top, canvas.right - 16, panel_top + 82};
        HBRUSH panel_brush = CreateSolidBrush(RGB(24, 45, 51));
        FillRect(dc, &panel, panel_brush);
        DeleteObject(panel_brush);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(70, 210, 200));
        const auto old_border = SelectObject(dc, border);
        const auto hollow = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
        SelectObject(dc, hollow);
        SelectObject(dc, old_border);
        DeleteObject(border);

        std::wostringstream text;
        text << L"坡度/坡向｜" << surface_source_name(slope_source_)
             << L"｜Z " << std::fixed << std::setprecision(3)
             << slope_analysis_.point.z << L"｜坡度 "
             << slope_analysis_.slope_degrees << L"°｜坡向 ";
        if ((slope_analysis_.flags & DT_SURFACE_ASPECT_UNDEFINED) != 0)
            text << L"平坦（无唯一方向）";
        else
            text << slope_analysis_.aspect_degrees << L"° "
                 << aspect_direction(slope_analysis_.aspect_degrees);
        text << L"\n∂Z/∂X " << slope_analysis_.dz_dx
             << L"｜∂Z/∂Y " << slope_analysis_.dz_dy
             << L"｜法向 (" << slope_analysis_.normal_x << L", "
             << slope_analysis_.normal_y << L", "
             << slope_analysis_.normal_z << L")";
        SetTextColor(dc, RGB(224, 250, 246));
        RECT text_rect{panel.left + 10, panel.top + 8,
                       panel.right - 10, panel.bottom - 6};
        DrawTextW(dc, text.str().c_str(), -1, &text_rect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    void paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        HBRUSH background = CreateSolidBrush(RGB(20, 25, 31));
        FillRect(dc, &client, background);
        DeleteObject(background);

        RECT toolbar = client;
        toolbar.bottom = kToolbarHeight;
        HBRUSH toolbar_brush = CreateSolidBrush(RGB(236, 239, 243));
        FillRect(dc, &toolbar, toolbar_brush);
        DeleteObject(toolbar_brush);

        const RECT canvas = canvas_rect();
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
        ensure_cache();
        if (view_mode_ == ViewMode::Terrain3D) {
            draw_mesh3d(dc, canvas);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(218, 228, 235));
            RECT help = canvas;
            help.left += 12;
            help.top += 10;
            DrawTextW(dc,
                      L"3D：左键环视｜右/中键平移｜滚轮缩放｜WASD/方向键漫游｜Q/E升降｜+/-垂直夸张｜Home复位",
                      -1, &help, DT_LEFT | DT_TOP | DT_SINGLELINE);
        } else {
            if (!panning_) refresh_grid_view_cache();
            draw_grid(dc);
            if (show_tin_) draw_mesh(dc);
            draw_cdt(dc);
            draw_triangle_set(dc, removed_triangles_, RGB(245, 68, 70), 2);
            draw_segment_set(dc, boundary_edges_, RGB(255, 210, 50), 3);
            draw_triangle_set(dc, added_triangles_, RGB(60, 220, 100), 2);
            draw_segment_set(dc, added_edges_, RGB(50, 255, 130), 3);
            draw_highlight(dc);
            draw_contours(dc);
            draw_box_zoom(dc);
            draw_measurement(dc, canvas);
            draw_slope_analysis(dc, canvas);
            draw_profile(dc, canvas);
        }
        RestoreDC(dc, saved);

        RECT status = client;
        status.top = std::max<LONG>(kToolbarHeight,
                                    client.bottom - kStatusHeight);
        HBRUSH status_brush = CreateSolidBrush(RGB(34, 41, 49));
        FillRect(dc, &status, status_brush);
        DeleteObject(status_brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(225, 230, 235));
        dt_statistics stats{};
        dt_get_statistics(mesh_, &stats);
        const wchar_t* mode = mode_ == Mode::Insert ? L"插入" :
                              mode_ == Mode::Delete ? L"删除" :
                              mode_ == Mode::ZoomBox ? L"框选放大" :
                              mode_ == Mode::CdtBreakline ? L"绘制断裂线" :
                              mode_ == Mode::CdtOuter ? L"绘制外边界" :
                              mode_ == Mode::CdtHole ? L"绘制孔洞" :
                              mode_ == Mode::CdtMoveVertex ? L"移动约束顶点" :
                              mode_ == Mode::CdtRemoveVertex ? L"删除约束顶点" :
                              mode_ == Mode::CdtDelete ? L"删除约束" :
                              mode_ == Mode::Profile ? L"任意剖面" :
                              mode_ == Mode::Measure ? L"面积/土方" :
                              mode_ == Mode::Slope ? L"坡度/坡向" : L"查询";
        std::wostringstream text;
        text << L"  视图: "
             << (view_mode_ == ViewMode::Terrain3D ? L"3D" : L"2D")
             << L" | 模式: " << mode << L" | 顶点: " << stats.vertex_count
             << L" | 三角形: " << stats.finite_triangle_count
             << L" | 当前窗口: " << triangles_.size()
             << (view_mode_ == ViewMode::Terrain3D ? L" | 高程×" : L" | 查询: ")
             << std::fixed << std::setprecision(
                    view_mode_ == ViewMode::Terrain3D ? 1 : 3)
             << (view_mode_ == ViewMode::Terrain3D ? z_exaggeration_ : last_query_ms_)
             << (view_mode_ == ViewMode::Terrain3D ? L"" : L" ms")
             << L" | 图层:"
             << (show_tin_ ? L"T" : L"-")
             << (show_grid_ && grid_ ? L"G" : L"-")
             << (show_contours_ && contours_ ? L"C" : L"-")
             << (show_cdt_ && cdt_info_.vertex_count != 0 ? L"D" : L"-")
             << L" | ";
        if (view_mode_ == ViewMode::Map2D && show_grid_ &&
            grid_preview_request_.task) {
            dt_task_info preview_info{};
            preview_info.struct_size = sizeof(preview_info);
            if (dt_task_get_info(grid_preview_request_.task, &preview_info) ==
                DT_OK) {
                text << L"异步LOD "
                     << static_cast<int>(std::clamp(preview_info.progress,
                                                    0.0, 1.0) * 100.0)
                     << L"% | ";
            }
        }
        if (view_mode_ == ViewMode::Map2D && show_grid_ &&
            grid_preview_source_width_ != 0 &&
            (grid_preview_source_width_ < grid_info_.width ||
             grid_preview_source_height_ < grid_info_.height)) {
            text << (grid_preview_used_tile_cache_ ? L"瓦片LOD " :
                     grid_preview_used_pyramid_ ? L"金字塔LOD " : L"局部LOD ")
                 << grid_preview_source_width_ << L"×"
                 << grid_preview_source_height_ << L"→"
                 << grid_preview_width_ << L"×" << grid_preview_height_
                 << L"";
            if (grid_preview_used_tile_cache_) {
                text << L" s" << grid_preview_lod_scale_ << L"，"
                     << grid_preview_reused_tile_count_ << L"/"
                     << grid_preview_tile_count_ << L"复用";
                if (grid_preview_used_disk_cache_) text << L"（磁盘）";
            }
            text << L" | ";
        }
        text << action_text_;
        DrawTextW(dc, text.str().c_str(), -1, &status,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(hwnd_, &ps);
    }

    std::wstring choose_file(bool save, const wchar_t* initial_name,
                             const wchar_t* filter,
                             const wchar_t* default_extension) const {
        wchar_t file[MAX_PATH]{};
        lstrcpynW(file, initial_name, MAX_PATH);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = file;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = default_extension;
        dialog.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const BOOL selected = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        return selected ? std::wstring(file) : std::wstring();
    }

    static std::wstring lower_extension(const std::wstring& file) {
        const auto dot = file.find_last_of(L'.');
        if (dot == std::wstring::npos) return {};
        std::wstring extension = file.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t ch) { return std::towlower(ch); });
        return extension;
    }

    static uint64_t persistent_file_revision(const std::wstring& file) {
        std::error_code error;
        const std::filesystem::path path(file);
        const uint64_t size = std::filesystem::file_size(path, error);
        if (error) return 0;
        const auto write_time = std::filesystem::last_write_time(path, error);
        if (error) return size;
        const auto ticks = write_time.time_since_epoch().count();
        uint64_t hash = 1469598103934665603ULL;
        const auto add = [&](const void* data, size_t bytes) {
            const auto* input = static_cast<const unsigned char*>(data);
            for (size_t index = 0; index < bytes; ++index) {
                hash ^= input[index];
                hash *= 1099511628211ULL;
            }
        };
        add(&size, sizeof(size));
        add(&ticks, sizeof(ticks));
        return hash;
    }

    void import_points_file() {
        const auto file = choose_file(
            false, L"terrain.xyz",
            L"XYZ 散点 (*.xyz;*.txt;*.csv)\0*.xyz;*.txt;*.csv\0所有文件 (*.*)\0*.*\0",
            L"xyz");
        if (file.empty()) return;
        const auto utf8 = wide_to_utf8(file.c_str());
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const auto status = dt_import_points_text(mesh_, utf8.c_str(), nullptr);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status == DT_OK) {
            show_tin_ = true;
            update_layer_menu();
            clear_derived_layers();
            clear_cdt_layer();
            clear_overlays();
            reset_view();
            invalidate_mesh_cache();
            dt_statistics stats{};
            dt_get_statistics(mesh_, &stats);
            const double ms =
                std::chrono::duration<double, std::milli>(end - begin).count();
            std::wostringstream text;
            text << L"散点导入并构网完成，" << stats.vertex_count
                 << L" 点，耗时 " << std::fixed << std::setprecision(1)
                 << ms << L" ms";
            action_text_ = text.str();
        } else {
            action_text_ = L"散点导入失败：" + last_error_text();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void save_file() {
        const auto file = choose_file(
            true, L"terrain.dtmesh",
            L"三角网文本 (*.dtmesh;*.txt)\0*.dtmesh;*.txt\0dterrain 二进制 (*.dtin)\0*.dtin\0所有文件 (*.*)\0*.*\0",
            L"dtmesh");
        if (file.empty()) return;
        const auto utf8 = wide_to_utf8(file.c_str());
        const auto extension = lower_extension(file);
        const auto status = extension == L".dtin"
                                ? dt_save(mesh_, utf8.c_str())
                                : dt_save_mesh_text(mesh_, utf8.c_str());
        if (status == DT_OK) action_text_ = L"已保存：" + file;
        else action_text_ = L"保存失败：" + last_error_text();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void load_file() {
        const auto file = choose_file(
            false, L"terrain.dtmesh",
            L"三角网文件 (*.dtmesh;*.txt;*.dtin)\0*.dtmesh;*.txt;*.dtin\0三角网文本 (*.dtmesh;*.txt)\0*.dtmesh;*.txt\0dterrain 二进制 (*.dtin)\0*.dtin\0所有文件 (*.*)\0*.*\0",
            L"dtmesh");
        if (file.empty()) return;
        const auto utf8 = wide_to_utf8(file.c_str());
        set_wait_cursor(true);
        const auto status = lower_extension(file) == L".dtin"
                                ? dt_load(mesh_, utf8.c_str(), nullptr)
                                : dt_load_mesh_text(mesh_, utf8.c_str(), nullptr);
        set_wait_cursor(false);
        if (status == DT_OK) {
            show_tin_ = true;
            update_layer_menu();
            clear_derived_layers();
            clear_cdt_layer();
            clear_overlays();
            reset_view();
            action_text_ = L"已加载：" + file;
            invalidate_mesh_cache();
        } else {
            action_text_ = L"加载失败：" + last_error_text();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void enter_2d_view() {
        if (view_mode_ == ViewMode::Map2D) return;
        view_mode_ = ViewMode::Map2D;
        SetWindowTextW(view_button_, L"切换3D");
        EnableWindow(exaggeration_button_, FALSE);
        end_drag3d();
    }

    static double nice_interval(double range) {
        if (!(range > 0.0) || !std::isfinite(range)) return 1.0;
        const double raw = range / 12.0;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
        const double normalized = raw / magnitude;
        const double factor = normalized <= 1.0 ? 1.0 :
                              normalized <= 2.0 ? 2.0 :
                              normalized <= 5.0 ? 5.0 : 10.0;
        return factor * magnitude;
    }

    bool tin_elevation_range(double& zmin, double& zmax) {
        dt_statistics stats{};
        if (dt_get_statistics(mesh_, &stats) != DT_OK || stats.dimension != 2) {
            action_text_ = L"当前没有可生成等高线的二维 TIN";
            return false;
        }
        show_tin_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        cache_valid_ = false;
        ensure_cache();
        if (triangles_.empty()) return false;
        zmin = std::numeric_limits<double>::max();
        zmax = std::numeric_limits<double>::lowest();
        for (const auto& triangle : triangles_)
            for (const auto& vertex : triangle.vertex) {
                zmin = std::min(zmin, vertex.point.z);
                zmax = std::max(zmax, vertex.point.z);
            }
        return zmax >= zmin;
    }

    bool cdt_elevation_range(double& zmin, double& zmax) {
        dt_cdt_statistics stats{};
        stats.struct_size = sizeof(stats);
        if (dt_cdt_get_statistics(cdt_, &stats) != DT_OK ||
            stats.domain_triangle_count == 0) {
            action_text_ = L"当前没有可生成等高线的约束网有效域";
            return false;
        }
        dt_cdt_query_result result = nullptr;
        if (dt_cdt_query_triangles(cdt_, &stats.bounds, &result) != DT_OK) {
            action_text_ = L"读取约束网有效域失败：" + last_error_text();
            return false;
        }
        dt_cdt_query_result_view triangles{};
        const dt_status status = dt_cdt_query_result_get_view(result, &triangles);
        if (status != DT_OK || triangles.triangle_count == 0) {
            dt_cdt_release_query_result(result);
            action_text_ = L"约束网有效域没有三角形";
            return false;
        }
        zmin = std::numeric_limits<double>::max();
        zmax = std::numeric_limits<double>::lowest();
        for (uint64_t index = 0; index < triangles.triangle_count; ++index)
            for (const auto& vertex : triangles.triangles[index].vertex) {
                zmin = std::min(zmin, vertex.point.z);
                zmax = std::max(zmax, vertex.point.z);
            }
        dt_cdt_release_query_result(result);
        return zmax >= zmin;
    }

    void create_cdt_from_tin() {
        dt_statistics stats{};
        if (dt_get_statistics(mesh_, &stats) != DT_OK || stats.dimension != 2) {
            action_text_ = L"创建约束网失败：当前没有二维 TIN";
            return;
        }
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_cdt_build_from_tin(cdt_, mesh_);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status == DT_OK) clear_analysis_for_source(ProfileSource::Cdt);
        if (status != DT_OK || !refresh_cdt_constraints()) {
            action_text_ = L"创建约束网失败：" + last_error_text();
            return;
        }
        cdt_draft_.clear();
        destroy_grid_layer();
        destroy_contour_layer();
        show_cdt_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已从 TIN 创建约束网：" << cdt_info_.base_point_count
             << L" 基础点，耗时 " << std::fixed << std::setprecision(1)
             << ms << L" ms；原约束已清除";
        action_text_ = text.str();
    }

    void convert_tin_to_grid() {
        dt_statistics stats{};
        if (dt_get_statistics(mesh_, &stats) != DT_OK || stats.dimension != 2) {
            action_text_ = L"TIN→GRID 失败：当前没有二维 TIN";
            return;
        }
        const double x_range = stats.bounds.xmax - stats.bounds.xmin;
        const double y_range = stats.bounds.ymax - stats.bounds.ymin;
        if (!(x_range > 0.0) || !(y_range > 0.0)) {
            action_text_ = L"TIN→GRID 失败：TIN 范围无效";
            return;
        }
        dt_tin_to_grid_options options{};
        options.struct_size = sizeof(options);
        if (x_range >= y_range) {
            options.width = 401;
            options.height = std::max<uint64_t>(2, static_cast<uint64_t>(
                std::llround(y_range / x_range * 400.0)) + 1);
        } else {
            options.height = 401;
            options.width = std::max<uint64_t>(2, static_cast<uint64_t>(
                std::llround(x_range / y_range * 400.0)) + 1);
        }
        options.geo_transform[0] = stats.bounds.xmin;
        options.geo_transform[1] = x_range / static_cast<double>(options.width - 1);
        options.geo_transform[3] = stats.bounds.ymin;
        options.geo_transform[5] = y_range / static_cast<double>(options.height - 1);
        options.nodata_value = -9999.0;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_from_tin(mesh_, &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"TIN→GRID 失败：" + last_error_text();
            return;
        }
        destroy_grid_layer();
        destroy_contour_layer();
        grid_ = output;
        show_grid_ = true;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"TIN→GRID 完成：" << grid_info_.width << L"×"
             << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void convert_cdt_to_grid() {
        dt_cdt_statistics stats{};
        stats.struct_size = sizeof(stats);
        if (dt_cdt_get_statistics(cdt_, &stats) != DT_OK ||
            stats.domain_triangle_count == 0) {
            action_text_ = L"约束网→GRID 失败：当前没有有效域";
            return;
        }
        const double x_range = stats.bounds.xmax - stats.bounds.xmin;
        const double y_range = stats.bounds.ymax - stats.bounds.ymin;
        if (!(x_range > 0.0) || !(y_range > 0.0)) {
            action_text_ = L"约束网→GRID 失败：范围无效";
            return;
        }
        dt_tin_to_grid_options options{};
        options.struct_size = sizeof(options);
        if (x_range >= y_range) {
            options.width = 401;
            options.height = std::max<uint64_t>(2, static_cast<uint64_t>(
                std::llround(y_range / x_range * 400.0)) + 1);
        } else {
            options.height = 401;
            options.width = std::max<uint64_t>(2, static_cast<uint64_t>(
                std::llround(x_range / y_range * 400.0)) + 1);
        }
        options.geo_transform[0] = stats.bounds.xmin;
        options.geo_transform[1] = x_range / static_cast<double>(options.width - 1);
        options.geo_transform[3] = stats.bounds.ymin;
        options.geo_transform[5] = y_range / static_cast<double>(options.height - 1);
        options.nodata_value = -9999.0;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_from_cdt(cdt_, &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"约束网→GRID 失败：" + last_error_text();
            return;
        }
        destroy_grid_layer();
        destroy_contour_layer();
        grid_ = output;
        show_grid_ = true;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"约束网→GRID 完成：" << grid_info_.width << L"×"
             << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，孔洞/域外为 NoData，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void convert_grid_to_tin() {
        if (!grid_) {
            action_text_ = L"GRID→TIN 失败：请先生成或导入 GRID";
            return;
        }
        dt_grid_to_tin_options options{};
        options.struct_size = sizeof(options);
        options.flags = DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_tin_from_grid(grid_, &options, mesh_);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"GRID→TIN 失败：" + last_error_text();
            return;
        }
        clear_analysis_for_source(ProfileSource::Tin);
        destroy_contour_layer();
        clear_overlays();
        show_tin_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        dt_statistics stats{};
        dt_get_statistics(mesh_, &stats);
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"GRID→TIN 完成：" << stats.vertex_count << L" 顶点，"
             << stats.finite_triangle_count << L" 面，耗时 "
             << std::fixed << std::setprecision(1) << ms
             << L" ms（已允许跨 NoData，孔洞需 CDT）";
        action_text_ = text.str();
    }

    void convert_contours_to_tin() {
        if (!contours_ || contour_info_.line_count == 0) {
            action_text_ = L"等高线→TIN 失败：请先生成或导入等高线";
            return;
        }
        dt_contours_to_tin_options options{};
        options.struct_size = sizeof(options);
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status =
            dt_tin_from_contours(contours_, &options, mesh_);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"等高线→TIN 失败：" + last_error_text();
            return;
        }
        clear_analysis_for_source(ProfileSource::Tin);
        destroy_grid_layer();
        clear_cdt_layer();
        clear_overlays();
        show_tin_ = true;
        show_contours_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        dt_statistics stats{};
        dt_get_statistics(mesh_, &stats);
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"等高线→TIN 完成：" << stats.vertex_count << L" 顶点，"
             << stats.finite_triangle_count << L" 面，耗时 " << std::fixed
             << std::setprecision(1) << ms
             << L" ms（普通 Delaunay，不强制保留等高线边）";
        action_text_ = text.str();
    }

    void convert_contours_to_grid() {
        if (!contours_ || contour_info_.line_count == 0) {
            action_text_ = L"等高线→GRID 失败：请先生成或导入等高线";
            return;
        }
        const double x_range =
            contour_info_.bounds.xmax - contour_info_.bounds.xmin;
        const double y_range =
            contour_info_.bounds.ymax - contour_info_.bounds.ymin;
        if (!(x_range > 0.0) || !(y_range > 0.0)) {
            action_text_ = L"等高线→GRID 失败：等高线范围无效";
            return;
        }
        dt_contours_to_tin_options tin_options{};
        tin_options.struct_size = sizeof(tin_options);
        dt_tin_to_grid_options grid_options{};
        grid_options.struct_size = sizeof(grid_options);
        if (x_range >= y_range) {
            grid_options.width = 401;
            grid_options.height = std::max<uint64_t>(
                2, static_cast<uint64_t>(
                       std::llround(y_range / x_range * 400.0)) +
                       1);
        } else {
            grid_options.height = 401;
            grid_options.width = std::max<uint64_t>(
                2, static_cast<uint64_t>(
                       std::llround(x_range / y_range * 400.0)) +
                       1);
        }
        grid_options.geo_transform[0] = contour_info_.bounds.xmin;
        grid_options.geo_transform[1] =
            x_range / static_cast<double>(grid_options.width - 1);
        grid_options.geo_transform[3] = contour_info_.bounds.ymin;
        grid_options.geo_transform[5] =
            y_range / static_cast<double>(grid_options.height - 1);
        grid_options.nodata_value = -9999.0;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_from_contours(
            contours_, &tin_options, &grid_options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"等高线→GRID 失败：" + last_error_text();
            return;
        }
        destroy_grid_layer();
        grid_ = output;
        show_grid_ = true;
        show_contours_ = true;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"等高线→GRID 完成：" << grid_info_.width << L"×"
             << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，耗时 " << std::fixed
             << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void generate_contours(bool from_grid) {
        double zmin = 0.0;
        double zmax = 0.0;
        if (from_grid) {
            if (!grid_) {
                action_text_ = L"生成等高线失败：请先生成或导入 GRID";
                return;
            }
            if (grid_preview_.empty() && !refresh_grid_cache()) {
                action_text_ = L"读取 GRID 失败：" + last_error_text();
                return;
            }
            if (grid_info_.valid_value_count == 0) {
                action_text_ = L"生成等高线失败：GRID 没有有效高程节点";
                return;
            }
            zmin = grid_zmin_;
            zmax = grid_zmax_;
        } else if (!tin_elevation_range(zmin, zmax)) {
            return;
        }
        const double interval = nice_interval(zmax - zmin);
        dt_contour_options options{};
        options.struct_size = sizeof(options);
        options.interval = interval;
        options.base = std::floor(zmin / interval) * interval;
        dt_contour_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = from_grid
            ? dt_contours_from_grid(grid_, &options, &output)
            : dt_contours_from_tin(mesh_, &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"生成等高线失败：" + last_error_text();
            return;
        }
        destroy_contour_layer();
        contours_ = output;
        contour_info_ = {};
        contour_info_.struct_size = sizeof(contour_info_);
        dt_contours_get_info(contours_, &contour_info_);
        show_contours_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << (from_grid ? L"GRID" : L"TIN") << L" 等高线完成：间隔 "
             << std::setprecision(8) << interval << L"，"
             << contour_info_.line_count << L" 条，"
             << contour_info_.vertex_count << L" 点，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void generate_cdt_contours() {
        double zmin = 0.0;
        double zmax = 0.0;
        if (!cdt_elevation_range(zmin, zmax)) return;
        const double interval = nice_interval(zmax - zmin);
        dt_contour_options options{};
        options.struct_size = sizeof(options);
        options.interval = interval;
        options.base = std::floor(zmin / interval) * interval;
        dt_contour_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_contours_from_cdt(cdt_, &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"约束网等高线失败：" + last_error_text();
            return;
        }
        destroy_contour_layer();
        contours_ = output;
        contour_info_ = {};
        contour_info_.struct_size = sizeof(contour_info_);
        dt_contours_get_info(contours_, &contour_info_);
        show_contours_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"约束网等高线完成：间隔 " << std::setprecision(8)
             << interval << L"，" << contour_info_.line_count << L" 条，"
             << contour_info_.vertex_count << L" 点，耗时 " << std::fixed
             << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void configure_terrain_parameters() {
        double z_factor = terrain_z_factor_;
        double azimuth = terrain_sun_azimuth_;
        double altitude = terrain_sun_altitude_;
        double workers = static_cast<double>(terrain_worker_count_);
        double tile_rows = static_cast<double>(terrain_tile_rows_);
        if (!prompt_double(hwnd_, L"专题分析参数",
                           L"高程单位系数 z-factor（必须大于 0）：",
                           z_factor))
            return;
        if (z_factor <= 0.0) {
            MessageBoxW(hwnd_, L"z-factor 必须大于 0。", L"无效参数",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (!prompt_double(hwnd_, L"阴影地形参数",
                           L"太阳方位角（度，北为 0，顺时针）：",
                           azimuth))
            return;
        if (!prompt_double(hwnd_, L"阴影地形参数",
                           L"太阳高度角（度，范围 -90 到 90）：",
                           altitude))
            return;
        if (altitude < -90.0 || altitude > 90.0) {
            MessageBoxW(hwnd_, L"太阳高度角必须在 -90 到 90 度之间。",
                        L"无效参数", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!prompt_double(hwnd_, L"专题计算性能",
                           L"工作线程数（0=自动，1=单线程，最大 64）：",
                           workers))
            return;
        if (workers < 0.0 || workers > 64.0 ||
            workers != std::floor(workers)) {
            MessageBoxW(hwnd_, L"线程数必须是 0 到 64 之间的整数。",
                        L"无效参数", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!prompt_double(hwnd_, L"专题计算性能",
                           L"分块行数（0=自动 64，最大 1048576）：",
                           tile_rows))
            return;
        if (tile_rows < 0.0 || tile_rows > 1048576.0 ||
            tile_rows != std::floor(tile_rows)) {
            MessageBoxW(hwnd_,
                        L"分块行数必须是 0 到 1048576 之间的整数。",
                        L"无效参数", MB_OK | MB_ICONWARNING);
            return;
        }
        azimuth = std::fmod(azimuth, 360.0);
        if (azimuth < 0.0) azimuth += 360.0;
        terrain_z_factor_ = z_factor;
        terrain_sun_azimuth_ = azimuth;
        terrain_sun_altitude_ = altitude;
        terrain_worker_count_ = static_cast<uint32_t>(workers);
        terrain_tile_rows_ = static_cast<uint32_t>(tile_rows);
        std::wostringstream text;
        text << L"专题参数已更新：z-factor=" << std::setprecision(8)
             << terrain_z_factor_ << L"，光照 " << terrain_sun_azimuth_
             << L"°/" << terrain_sun_altitude_ << L"°，线程=";
        if (terrain_worker_count_ == 0)
            text << L"自动";
        else
            text << terrain_worker_count_;
        text << L"，块高=";
        if (terrain_tile_rows_ == 0)
            text << L"自动(64)";
        else
            text << terrain_tile_rows_;
        action_text_ = text.str();
    }

    std::string grid_crs_wkt(dt_grid_handle grid) const {
        size_t required = 0;
        if (!grid || dt_grid_get_crs_wkt(grid, nullptr, 0, &required) != DT_OK ||
            required <= 1) {
            return {};
        }
        std::vector<char> buffer(required);
        if (dt_grid_get_crs_wkt(grid, buffer.data(), buffer.size(), nullptr) !=
            DT_OK) {
            return {};
        }
        return buffer.data();
    }

    bool earthwork_grids_compatible(dt_grid_handle design,
                                    std::wstring& reason) const {
        dt_grid_info existing_info{};
        dt_grid_info design_info{};
        existing_info.struct_size = sizeof(existing_info);
        design_info.struct_size = sizeof(design_info);
        if (!grid_ || !design ||
            dt_grid_get_info(grid_, &existing_info) != DT_OK ||
            dt_grid_get_info(design, &design_info) != DT_OK) {
            reason = L"无法读取 GRID 元数据";
            return false;
        }
        if (existing_info.width != design_info.width ||
            existing_info.height != design_info.height) {
            reason = L"行列数不一致";
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            const double a = existing_info.geo_transform[i];
            const double b = design_info.geo_transform[i];
            if (std::abs(a - b) >
                1e-12 * std::max({1.0, std::abs(a), std::abs(b)})) {
                reason = L"六参数仿射变换不一致";
                return false;
            }
        }
        if (grid_crs_wkt(grid_) != grid_crs_wkt(design)) {
            reason = L"坐标参考系不一致";
            return false;
        }
        return true;
    }

    void clip_grid_with_measurement(uint32_t flags) {
        if (!grid_) {
            action_text_ = L"GRID 区域处理失败：当前没有高程 GRID";
            return;
        }
        if (!measurement_complete_ || measurement_polygon_.size() < 3) {
            action_text_ = L"GRID 区域处理失败：请先完成一个面积/土方量测多边形";
            return;
        }
        dt_grid_clip_options options{};
        options.struct_size = sizeof(options);
        options.flags = flags;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        options.output_nodata_value = -9999.0;
        dt_task_handle task = nullptr;
        const dt_status start_status = dt_grid_clip_polygon_async(
            grid_, measurement_polygon_.data(), measurement_polygon_.size(),
            &options, &task);
        if (start_status != DT_OK) {
            action_text_ = L"GRID 区域处理启动失败：" + last_error_text();
            return;
        }
        const wchar_t* operation =
            (flags & DT_GRID_CLIP_CROP_TO_BOUNDS) != 0
                ? L"紧凑裁剪"
                : (flags & DT_GRID_CLIP_INVERT) != 0
                      ? L"反向掩膜"
                      : L"区域掩膜";
        terrain_task_ = task;
        terrain_task_running_ = true;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        int32_t completed = 0;
        int quit_code = 0;
        bool saw_quit = false;
        dt_task_info task_info{};
        while (!completed) {
            if (dt_task_wait(task, 25, &completed) != DT_OK) {
                dt_task_request_cancel(task);
                break;
            }
            if (dt_task_get_info(task, &task_info) == DT_OK) {
                const int percent = static_cast<int>(std::clamp(
                    task_info.progress * 100.0, 0.0, 100.0));
                std::wostringstream status;
                status << L"正在" << operation << L" GRID：" << percent
                       << L"%（Esc 可取消）";
                if (task_info.cancellation_requested) status << L"，正在取消…";
                action_text_ = status.str();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    saw_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                    dt_task_request_cancel(task);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!completed) dt_task_wait(task, UINT32_MAX, &completed);
        dt_task_get_info(task, &task_info);
        dt_grid_handle output = nullptr;
        dt_status result_status = task_info.result_status;
        if (task_info.state == DT_TASK_SUCCEEDED)
            result_status = dt_task_get_grid_result(task, &output);
        std::wstring task_error;
        if (task_info.state == DT_TASK_FAILED) {
            char buffer[1024]{};
            if (dt_task_get_error(task, buffer, sizeof(buffer), nullptr) == DT_OK)
                task_error = utf8_to_wide(buffer);
        }
        terrain_task_running_ = false;
        terrain_task_ = nullptr;
        dt_task_destroy(task);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (task_info.state == DT_TASK_CANCELLED) {
            action_text_ = std::wstring(L"GRID ") + operation +
                           L"已取消；原 GRID 保持不变";
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        if (result_status != DT_OK || !output) {
            dt_grid_destroy(output);
            action_text_ = std::wstring(L"GRID ") + operation + L"失败：" +
                           (task_error.empty() ? last_error_text() : task_error);
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        destroy_grid_layer();
        destroy_contour_layer();
        clear_measurement();
        grid_ = output;
        show_grid_ = true;
        grid_theme_ = GridTheme::Elevation;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        fit_view_to_bounds(grid_info_.bounds, 1.08);
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"GRID " << operation << L"完成：" << grid_info_.width
             << L"×" << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，耗时 " << std::fixed
             << std::setprecision(1) << ms << L" ms；量测多边形已清除";
        if (grid_info_.width > grid_preview_width_ ||
            grid_info_.height > grid_preview_height_)
            text << L"；LOD 预览 " << grid_preview_width_ << L"×"
                 << grid_preview_height_;
        action_text_ = text.str();
        if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (saw_quit) PostQuitMessage(quit_code);
    }

    void clear_earthwork_output(bool clear_design) {
        if (grid_theme_ == GridTheme::Difference) {
            dt_grid_destroy(terrain_grid_);
            terrain_grid_ = nullptr;
            grid_theme_ = GridTheme::Elevation;
            refresh_grid_cache();
        }
        earthwork_result_ = {};
        earthwork_result_valid_ = false;
        if (clear_design) {
            dt_grid_destroy(design_grid_);
            design_grid_ = nullptr;
        }
        update_layer_menu();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void load_earthwork_design(bool gdal) {
        if (!grid_) {
            action_text_ = L"加载设计面失败：请先生成或导入现状 GRID";
            return;
        }
        if (gdal && !gdal_gtiff_available_) {
            action_text_ = L"当前构建未启用 GDAL GeoTIFF 驱动";
            return;
        }
        const auto file = gdal
            ? choose_file(false, L"design.tif",
                  L"GeoTIFF/COG (*.tif;*.tiff)\0*.tif;*.tiff\0所有文件 (*.*)\0*.*\0",
                  L"tif")
            : choose_file(false, L"design.dgrid",
                  L"DGRID 规则网格 (*.dgrid;*.txt)\0*.dgrid;*.txt\0所有文件 (*.*)\0*.*\0",
                  L"dgrid");
        if (file.empty()) return;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        dt_status status = DT_OK;
        if (gdal) {
            dt_gdal_raster_load_options options{};
            options.struct_size = sizeof(options);
            options.band_index = 1;
            status = dt_grid_load_gdal_raster(
                wide_to_utf8(file.c_str()).c_str(), &options, &output);
        } else {
            status = dt_grid_load_text(wide_to_utf8(file.c_str()).c_str(),
                                       &output);
        }
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"设计面加载失败：" + last_error_text();
            return;
        }
        if (grid_crs_wkt(grid_) != grid_crs_wkt(output)) {
            dt_grid_destroy(output);
            action_text_ = L"设计面与现状面不兼容：坐标参考系不一致；本程序不会隐式重投影";
            return;
        }
        clear_earthwork_output(true);
        design_grid_ = output;
        dt_grid_info info{};
        info.struct_size = sizeof(info);
        dt_grid_get_info(design_grid_, &info);
        std::wostringstream text;
        text << L"已加载设计面：" << info.width << L"×" << info.height
             << L"，有效节点 " << info.valid_value_count;
        std::wstring reason;
        if (earthwork_grids_compatible(design_grid_, reason)) {
            text << L"；已与现状面节点对齐，可直接计算挖填方";
        } else {
            text << L"；" << reason
                 << L"，请显式选择双线性或最近邻对齐";
        }
        action_text_ = text.str();
    }

    void resample_earthwork_design(uint32_t method) {
        if (!grid_ || !design_grid_) {
            action_text_ = L"设计面对齐失败：需要现状 GRID 和已加载设计面";
            return;
        }
        if (grid_crs_wkt(grid_) != grid_crs_wkt(design_grid_)) {
            action_text_ = L"设计面对齐失败：坐标参考系不一致，本程序不执行隐式重投影";
            return;
        }
        std::wstring compatibility_reason;
        if (earthwork_grids_compatible(design_grid_, compatibility_reason)) {
            action_text_ = L"设计面已经与现状 GRID 节点对齐，无需重采样";
            return;
        }
        dt_grid_resample_options options{};
        options.struct_size = sizeof(options);
        options.method = method;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        options.output_nodata_value = -9999.0;
        dt_task_handle task = nullptr;
        const dt_status start_status = dt_grid_resample_like_async(
            design_grid_, grid_, &options, &task);
        if (start_status != DT_OK) {
            action_text_ = L"设计面对齐启动失败：" + last_error_text();
            return;
        }
        clear_earthwork_output(false);
        terrain_task_ = task;
        terrain_task_running_ = true;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        int32_t completed = 0;
        int quit_code = 0;
        bool saw_quit = false;
        dt_task_info task_info{};
        while (!completed) {
            if (dt_task_wait(task, 25, &completed) != DT_OK) {
                dt_task_request_cancel(task);
                break;
            }
            if (dt_task_get_info(task, &task_info) == DT_OK) {
                const int percent = static_cast<int>(std::clamp(
                    task_info.progress * 100.0, 0.0, 100.0));
                std::wostringstream status;
                status << L"正在"
                       << (method == DT_GRID_RESAMPLE_NEAREST
                               ? L"最近邻"
                               : L"双线性")
                       << L"对齐设计面：" << percent << L"%（Esc 可取消）";
                if (task_info.cancellation_requested) status << L"，正在取消…";
                action_text_ = status.str();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    saw_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                    dt_task_request_cancel(task);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!completed) dt_task_wait(task, UINT32_MAX, &completed);
        dt_task_get_info(task, &task_info);
        dt_grid_handle aligned = nullptr;
        dt_status result_status = task_info.result_status;
        if (task_info.state == DT_TASK_SUCCEEDED)
            result_status = dt_task_get_grid_result(task, &aligned);
        std::wstring task_error;
        if (task_info.state == DT_TASK_FAILED) {
            char buffer[1024]{};
            if (dt_task_get_error(task, buffer, sizeof(buffer), nullptr) == DT_OK)
                task_error = utf8_to_wide(buffer);
        }
        terrain_task_running_ = false;
        terrain_task_ = nullptr;
        dt_task_destroy(task);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (task_info.state == DT_TASK_CANCELLED) {
            action_text_ = L"设计面对齐已取消；原设计面保持不变";
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        if (result_status != DT_OK || !aligned) {
            dt_grid_destroy(aligned);
            action_text_ = L"设计面对齐失败：" +
                           (task_error.empty() ? last_error_text() : task_error);
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        dt_grid_destroy(design_grid_);
        design_grid_ = aligned;
        dt_grid_info info{};
        info.struct_size = sizeof(info);
        dt_grid_get_info(design_grid_, &info);
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << (method == DT_GRID_RESAMPLE_NEAREST ? L"最近邻" : L"双线性")
             << L"设计面对齐完成：" << info.width << L"×" << info.height
             << L"，有效节点 " << info.valid_value_count
             << L"，耗时 " << std::fixed << std::setprecision(1) << ms
             << L" ms；现在可计算双表面挖填方";
        action_text_ = text.str();
        update_layer_menu();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (saw_quit) PostQuitMessage(quit_code);
    }

    void create_offset_design_grid() {
        if (!grid_ || !refresh_grid_cache()) {
            action_text_ = L"创建设计面失败：请先生成或导入现状 GRID";
            return;
        }
        double offset = 5.0;
        if (!prompt_double(hwnd_, L"创建偏移设计面",
                           L"设计面相对现状面的高程偏移（正值表示填高）：",
                           offset)) {
            return;
        }
        dt_grid_create_options create{};
        create.struct_size = sizeof(create);
        create.flags = grid_info_.flags & DT_GRID_HAS_NODATA;
        create.width = grid_info_.width;
        create.height = grid_info_.height;
        std::copy(std::begin(grid_info_.geo_transform),
                  std::end(grid_info_.geo_transform), create.geo_transform);
        create.nodata_value = grid_info_.nodata_value;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        dt_status status = dt_grid_create(&create, &output);
        std::vector<double> row;
        try {
            row.resize(static_cast<size_t>(grid_info_.width));
        } catch (...) {
            status = DT_E_OUT_OF_MEMORY;
        }
        for (uint64_t y = 0; status == DT_OK && y < grid_info_.height; ++y) {
            status = dt_grid_read_window(grid_, 0, y, grid_info_.width, 1,
                                         row.data(), grid_info_.width);
            if (status != DT_OK) break;
            for (double& value : row) {
                if (!is_grid_nodata(grid_info_, value)) value += offset;
            }
            status = dt_grid_write_window(output, 0, y, grid_info_.width, 1,
                                          row.data(), grid_info_.width);
        }
        if (status == DT_OK) {
            const std::string crs = grid_crs_wkt(grid_);
            status = dt_grid_set_crs_wkt(output, crs.c_str());
        }
        set_wait_cursor(false);
        if (status != DT_OK) {
            dt_grid_destroy(output);
            action_text_ = L"创建偏移设计面失败：" + last_error_text();
            return;
        }
        clear_earthwork_output(true);
        design_grid_ = output;
        std::wostringstream text;
        text << L"已从现状 GRID 创建设计面，高程偏移 "
             << std::setprecision(8) << offset
             << L"；正偏移将在分析中计为填方";
        action_text_ = text.str();
    }

    void run_earthwork_analysis() {
        if (!grid_ || !design_grid_) {
            action_text_ = L"双表面分析失败：需要现状 GRID 和设计面 GRID";
            return;
        }
        std::wstring reason;
        if (!earthwork_grids_compatible(design_grid_, reason)) {
            action_text_ = L"双表面分析失败：" + reason +
                           L"；请先显式对齐设计面";
            return;
        }
        dt_grid_earthwork_options options{};
        options.struct_size = sizeof(options);
        options.flags = DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        options.existing_z_factor = terrain_z_factor_;
        options.design_z_factor = terrain_z_factor_;
        options.output_nodata_value = -9999.0;
        dt_task_handle task = nullptr;
        const dt_status start_status = dt_grid_compare_earthwork_async(
            grid_, design_grid_, &options, &task);
        if (start_status != DT_OK) {
            action_text_ = L"双表面分析启动失败：" + last_error_text();
            return;
        }
        terrain_task_ = task;
        terrain_task_running_ = true;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        int32_t completed = 0;
        int quit_code = 0;
        bool saw_quit = false;
        dt_task_info task_info{};
        while (!completed) {
            if (dt_task_wait(task, 25, &completed) != DT_OK) {
                dt_task_request_cancel(task);
                break;
            }
            if (dt_task_get_info(task, &task_info) == DT_OK) {
                const int percent = static_cast<int>(std::clamp(
                    task_info.progress * 100.0, 0.0, 100.0));
                std::wostringstream status;
                status << L"正在计算双表面挖填方：" << percent
                       << L"%（Esc 可取消）";
                if (task_info.cancellation_requested) status << L"，正在取消…";
                action_text_ = status.str();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    saw_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                    dt_task_request_cancel(task);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!completed) dt_task_wait(task, UINT32_MAX, &completed);
        dt_task_get_info(task, &task_info);
        dt_grid_earthwork_result result{};
        dt_grid_handle difference = nullptr;
        dt_status result_status = task_info.result_status;
        if (task_info.state == DT_TASK_SUCCEEDED) {
            result_status = dt_task_get_earthwork_result(
                task, &result, &difference);
        }
        std::wstring task_error;
        if (task_info.state == DT_TASK_FAILED) {
            char buffer[1024]{};
            if (dt_task_get_error(task, buffer, sizeof(buffer), nullptr) == DT_OK)
                task_error = utf8_to_wide(buffer);
        }
        terrain_task_running_ = false;
        terrain_task_ = nullptr;
        dt_task_destroy(task);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (task_info.state == DT_TASK_CANCELLED) {
            action_text_ = L"双表面挖填方计算已取消";
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        if (result_status != DT_OK || !difference) {
            dt_grid_destroy(difference);
            action_text_ = L"双表面分析失败：" +
                           (task_error.empty() ? last_error_text() : task_error);
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        dt_grid_destroy(terrain_grid_);
        terrain_grid_ = difference;
        earthwork_result_ = result;
        earthwork_result_valid_ = true;
        grid_theme_ = GridTheme::Difference;
        show_grid_ = true;
        refresh_terrain_cache();
        update_layer_menu();
        enter_2d_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << std::fixed << std::setprecision(3)
             << L"双表面土方完成：挖方 " << result.cut_volume
             << L"，填方 " << result.fill_volume
             << L"，净方(挖-填) " << result.net_volume
             << L"，有效覆盖 " << result.coverage_ratio * 100.0
             << L"%，耗时 " << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
        if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (saw_quit) PostQuitMessage(quit_code);
    }

    void export_earthwork_csv() {
        if (!earthwork_result_valid_) {
            action_text_ = L"没有可导出的双表面土方结果";
            return;
        }
        const auto file = choose_file(
            true, L"earthwork_summary.csv",
            L"CSV 土方摘要 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0", L"csv");
        if (file.empty()) return;
        std::ostringstream output;
        output << std::setprecision(17)
               << "metric,value\r\n"
               << "cell_count," << earthwork_result_.cell_count << "\r\n"
               << "valid_triangle_count," << earthwork_result_.valid_triangle_count << "\r\n"
               << "skipped_triangle_count," << earthwork_result_.skipped_triangle_count << "\r\n"
               << "total_plan_area," << earthwork_result_.total_plan_area << "\r\n"
               << "valid_plan_area," << earthwork_result_.valid_plan_area << "\r\n"
               << "coverage_ratio," << earthwork_result_.coverage_ratio << "\r\n"
               << "cut_volume," << earthwork_result_.cut_volume << "\r\n"
               << "fill_volume," << earthwork_result_.fill_volume << "\r\n"
               << "net_volume," << earthwork_result_.net_volume << "\r\n"
               << "minimum_difference," << earthwork_result_.minimum_difference << "\r\n"
               << "maximum_difference," << earthwork_result_.maximum_difference << "\r\n"
               << "mean_difference," << earthwork_result_.mean_difference << "\r\n"
               << "rmse_difference," << earthwork_result_.rmse_difference << "\r\n";
        const std::string bytes = output.str();
        FILE* stream = _wfopen(file.c_str(), L"wb");
        if (!stream) {
            action_text_ = L"土方摘要导出失败：无法创建文件";
            return;
        }
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), stream);
        const int close_status = std::fclose(stream);
        action_text_ = written == bytes.size() && close_status == 0
            ? L"已导出双表面土方摘要：" + file
            : L"土方摘要导出失败：文件写入不完整";
    }

    void derive_terrain_grid(uint32_t kind) {
        if (!grid_) {
            dt_statistics statistics{};
            dt_get_statistics(mesh_, &statistics);
            if (cdt_ && cdt_info_.domain_triangle_count > 0) {
                convert_cdt_to_grid();
            } else if (statistics.finite_triangle_count > 0) {
                convert_tin_to_grid();
            }
        }
        if (!grid_) {
            action_text_ = L"专题分析失败：请先导入 GRID，或构建 TIN/CDT";
            return;
        }
        dt_grid_terrain_options options{};
        options.struct_size = sizeof(options);
        options.kind = kind;
        options.z_factor = terrain_z_factor_;
        options.sun_azimuth_degrees = terrain_sun_azimuth_;
        options.sun_altitude_degrees = terrain_sun_altitude_;
        options.output_nodata_value = -9999.0;
        options.worker_count = terrain_worker_count_;
        options.tile_row_count = terrain_tile_rows_;
        const wchar_t* requested_name =
            kind == DT_GRID_TERRAIN_SLOPE_DEGREES
                ? L"坡度"
                : kind == DT_GRID_TERRAIN_ASPECT_DEGREES ? L"坡向"
                                                         : L"阴影地形";
        dt_task_handle task = nullptr;
        const dt_status start_status =
            dt_grid_derive_terrain_async(grid_, &options, &task);
        if (start_status != DT_OK) {
            action_text_ = L"专题分析启动失败：" + last_error_text();
            return;
        }
        terrain_task_ = task;
        terrain_task_running_ = true;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        int32_t completed = 0;
        int quit_code = 0;
        bool saw_quit = false;
        dt_task_info task_info{};
        while (!completed) {
            const dt_status wait_status = dt_task_wait(task, 25, &completed);
            if (wait_status != DT_OK) {
                dt_task_request_cancel(task);
                break;
            }
            if (dt_task_get_info(task, &task_info) == DT_OK) {
                const int percent = static_cast<int>(std::clamp(
                    task_info.progress * 100.0, 0.0, 100.0));
                std::wostringstream status;
                status << L"正在计算" << requested_name << L"专题图："
                       << percent << L"%（Esc 可取消）";
                if (task_info.cancellation_requested)
                    status << L"，正在取消…";
                action_text_ = status.str();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    saw_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                    dt_task_request_cancel(task);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!completed) dt_task_wait(task, UINT32_MAX, &completed);
        dt_task_get_info(task, &task_info);
        dt_grid_handle output = nullptr;
        dt_status result_status = task_info.result_status;
        if (task_info.state == DT_TASK_SUCCEEDED)
            result_status = dt_task_get_grid_result(task, &output);
        std::wstring task_error;
        if (task_info.state == DT_TASK_FAILED) {
            char buffer[1024]{};
            if (dt_task_get_error(task, buffer, sizeof(buffer), nullptr) == DT_OK)
                task_error = utf8_to_wide(buffer);
        }
        terrain_task_running_ = false;
        terrain_task_ = nullptr;
        dt_task_destroy(task);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (task_info.state == DT_TASK_CANCELLED) {
            action_text_ = std::wstring(requested_name) + L"专题计算已取消";
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        if (result_status != DT_OK || !output) {
            action_text_ = L"专题分析失败：" +
                           (task_error.empty() ? last_error_text() : task_error);
            if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            if (saw_quit) PostQuitMessage(quit_code);
            return;
        }
        dt_grid_destroy(terrain_grid_);
        terrain_grid_ = output;
        grid_theme_ = kind == DT_GRID_TERRAIN_SLOPE_DEGREES
                          ? GridTheme::Slope
                          : kind == DT_GRID_TERRAIN_ASPECT_DEGREES
                                ? GridTheme::Aspect
                                : GridTheme::Hillshade;
        show_grid_ = true;
        refresh_terrain_cache();
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        dt_grid_info info{};
        info.struct_size = sizeof(info);
        dt_grid_get_info(terrain_grid_, &info);
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        const wchar_t* name = grid_theme_ == GridTheme::Slope
                                  ? L"坡度"
                                  : grid_theme_ == GridTheme::Aspect
                                        ? L"坡向"
                                        : L"阴影地形";
        std::wostringstream text;
        text << name << L"专题图完成：" << info.width << L"×" << info.height
             << L"，有效节点 " << info.valid_value_count << L"，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        text << L"，z-factor=" << std::setprecision(4) << terrain_z_factor_;
        if (grid_theme_ == GridTheme::Hillshade)
            text << L"，光照 " << terrain_sun_azimuth_ << L"°/"
                 << terrain_sun_altitude_ << L"°";
        text << L"，线程=";
        if (terrain_worker_count_ == 0)
            text << L"自动";
        else
            text << terrain_worker_count_;
        text << L"，块高="
             << (terrain_tile_rows_ == 0 ? 64U : terrain_tile_rows_);
        if (!grid_preview_.empty())
            text << L"，LOD 预览 " << grid_preview_width_ << L"×"
                 << grid_preview_height_;
        action_text_ = text.str();
        if (close_after_terrain_task_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (saw_quit) PostQuitMessage(quit_code);
    }

    void show_elevation_grid() {
        if (!grid_) {
            action_text_ = L"当前没有高程 GRID";
            return;
        }
        dt_grid_destroy(terrain_grid_);
        terrain_grid_ = nullptr;
        grid_theme_ = GridTheme::Elevation;
        refresh_grid_cache();
        show_grid_ = true;
        update_layer_menu();
        enter_2d_view();
        InvalidateRect(hwnd_, nullptr, FALSE);
        action_text_ = L"已恢复显示高程 GRID";
    }

    void export_terrain_grid_file() {
        if (!terrain_grid_) {
            action_text_ = L"没有可导出的专题图，请先生成坡度、坡向或阴影地形图";
            return;
        }
        const auto file = choose_file(
            true, L"terrain_analysis.dgrid",
            L"DGRID 规则网格 (*.dgrid;*.txt)\0*.dgrid;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dgrid");
        if (file.empty()) return;
        const dt_status status = dt_grid_save_text(
            terrain_grid_, wide_to_utf8(file.c_str()).c_str());
        action_text_ = status == DT_OK ? L"已导出专题图：" + file
                                       : L"专题图导出失败：" + last_error_text();
    }

    void export_terrain_geotiff() {
        if (!terrain_grid_) {
            action_text_ = L"没有可导出的专题图，请先生成分析结果";
            return;
        }
        if (!gdal_gtiff_available_) {
            action_text_ = L"当前构建未启用 GDAL GeoTIFF 驱动";
            return;
        }
        const auto file = choose_file(
            true, L"terrain_analysis.tif",
            L"GeoTIFF (*.tif;*.tiff)\0*.tif;*.tiff\0所有文件 (*.*)\0*.*\0",
            L"tif");
        if (file.empty()) return;
        const char* creation_options[] = {
            "TILED=YES", "COMPRESS=DEFLATE", "BIGTIFF=IF_SAFER", nullptr};
        dt_gdal_raster_save_options options{};
        options.struct_size = sizeof(options);
        options.driver_name = "GTiff";
        options.creation_options = creation_options;
        set_wait_cursor(true);
        const dt_status status = dt_grid_save_gdal_raster(
            terrain_grid_, wide_to_utf8(file.c_str()).c_str(), &options);
        set_wait_cursor(false);
        action_text_ = status == DT_OK ? L"已导出专题 GeoTIFF：" + file
                                       : L"专题 GeoTIFF 导出失败：" + last_error_text();
    }

    void import_grid_file() {
        const auto file = choose_file(false, L"terrain.dgridb",
            L"DGRIDB 映射网格 (*.dgridb)\0*.dgridb\0DGRID 文本 (*.dgrid;*.txt)\0*.dgrid;*.txt\0所有 GRID (*.dgridb;*.dgrid;*.txt)\0*.dgridb;*.dgrid;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dgridb");
        if (file.empty()) return;
        dt_grid_handle output = nullptr;
        const bool binary = lower_extension(file) == L".dgridb";
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const auto utf8 = wide_to_utf8(file.c_str());
        const dt_status status = binary
            ? dt_grid_load_binary(utf8.c_str(), &output)
            : dt_grid_load_text(utf8.c_str(), &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"GRID 导入失败：" + last_error_text();
            return;
        }
        destroy_grid_layer();
        grid_ = output;
        if (binary) {
            grid_disk_cache_file_ = utf8 + ".dgtile";
            grid_disk_cache_revision_ = persistent_file_revision(file);
        }
        show_grid_ = true;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"已导入 GRID：" << grid_info_.width << L"×"
             << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，耗时 " << std::fixed
             << std::setprecision(1)
             << std::chrono::duration<double, std::milli>(end - begin).count()
             << L" ms";
        if ((grid_info_.flags & DT_GRID_STORAGE_MEMORY_MAPPED) != 0)
            text << L"；写时复制映射";
        if ((grid_info_.flags & DT_GRID_HAS_PERSISTENT_OVERVIEW) != 0)
            text << L"；内置概览";
        if ((grid_info_.flags & DT_GRID_HAS_PYRAMID) != 0)
            text << L"；多级金字塔";
        if ((grid_info_.flags & DT_GRID_HAS_BLOCK_CHECKSUMS) != 0)
            text << L"；按需块校验缓存";
        if (binary) text << L"；DGTILE 跨会话瓦片";
        if (grid_info_.width > grid_preview_width_ ||
            grid_info_.height > grid_preview_height_)
            text << L"；LOD 预览 " << grid_preview_width_ << L"×"
                 << grid_preview_height_;
        action_text_ = text.str();
    }

    void export_grid_file() {
        if (!grid_) {
            action_text_ = L"没有可导出的 GRID";
            return;
        }
        const auto file = choose_file(true, L"terrain.dgridb",
            L"DGRIDB 映射网格 (*.dgridb)\0*.dgridb\0DGRID 文本 (*.dgrid;*.txt)\0*.dgrid;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dgridb");
        if (file.empty()) return;
        const bool binary = lower_extension(file) == L".dgridb";
        const auto utf8 = wide_to_utf8(file.c_str());
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = binary
            ? dt_grid_save_binary(grid_, utf8.c_str())
            : dt_grid_save_text(grid_, utf8.c_str());
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status == DT_OK) {
            if (binary) {
                cancel_grid_preview_tasks(false);
                destroy_grid_view_cache();
                grid_disk_cache_file_ = utf8 + ".dgtile";
                grid_disk_cache_revision_ = persistent_file_revision(file);
                invalidate_grid_view_cache();
            }
            std::wostringstream text;
            text << L"已导出 " << (binary ? L"DGRIDB：" : L"DGRID：")
                 << file << L"，耗时 " << std::fixed << std::setprecision(1)
                 << std::chrono::duration<double, std::milli>(end - begin).count()
                 << L" ms";
            action_text_ = text.str();
        } else {
            action_text_ = L"GRID 导出失败：" + last_error_text();
        }
    }

    void verify_dgridb_file() {
        const auto file = choose_file(false, L"terrain.dgridb",
            L"DGRIDB 映射网格 (*.dgridb)\0*.dgridb\0所有文件 (*.*)\0*.*\0",
            L"dgridb");
        if (file.empty()) return;
        const auto utf8 = wide_to_utf8(file.c_str());
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_verify_binary_file(utf8.c_str());
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status == DT_OK) {
            std::wostringstream text;
            text << L"DGRIDB 数据块校验通过：" << file << L"，耗时 "
                 << std::fixed << std::setprecision(1)
                 << std::chrono::duration<double, std::milli>(end - begin).count()
                 << L" ms";
            action_text_ = text.str();
        } else {
            action_text_ = L"DGRIDB 校验失败：" + last_error_text();
        }
    }

    void compact_current_dgtile() {
        if (!grid_ || grid_disk_cache_file_.empty()) {
            action_text_ = L"当前 GRID 没有可维护的 DGTILE 显示缓存";
            return;
        }
        cancel_grid_preview_tasks(true);
        destroy_grid_view_cache();
        if (!ensure_grid_view_cache(grid_)) {
            action_text_ = L"无法打开当前 DGTILE 显示缓存：" +
                           last_error_text();
            return;
        }
        dt_grid_view_cache_compact_result result{};
        result.struct_size = sizeof(result);
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_view_cache_compact(
            grid_view_cache_, &result);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"DGTILE 压缩失败：" + last_error_text();
            return;
        }
        const double mib = 1024.0 * 1024.0;
        std::wostringstream text;
        text << L"DGTILE 压缩完成："
             << std::fixed << std::setprecision(2)
             << static_cast<double>(result.input_file_bytes) / mib << L" → "
             << static_cast<double>(result.output_file_bytes) / mib << L" MiB，回收 "
             << static_cast<double>(result.reclaimed_bytes) / mib << L" MiB；保留 "
             << result.retained_tile_count << L" 块，移除重复记录 "
             << result.dropped_duplicate_record_count << L" 条、损坏块 "
             << result.dropped_corrupt_tile_count << L" 块；耗时 "
             << std::setprecision(1)
             << std::chrono::duration<double, std::milli>(end - begin).count()
             << L" ms";
        action_text_ = text.str();
        invalidate_grid_view_cache();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void import_contour_file() {
        const auto file = choose_file(false, L"terrain.dcontour",
            L"DCONTOUR 等高线 (*.dcontour;*.txt)\0*.dcontour;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dcontour");
        if (file.empty()) return;
        dt_contour_handle output = nullptr;
        set_wait_cursor(true);
        const dt_status status = dt_contours_load_text(
            wide_to_utf8(file.c_str()).c_str(), &output);
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"等高线导入失败：" + last_error_text();
            return;
        }
        destroy_contour_layer();
        contours_ = output;
        contour_info_ = {};
        contour_info_.struct_size = sizeof(contour_info_);
        dt_contours_get_info(contours_, &contour_info_);
        show_contours_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        std::wostringstream text;
        text << L"已导入等高线：" << contour_info_.line_count << L" 条，"
             << contour_info_.vertex_count << L" 点";
        action_text_ = text.str();
    }

    void export_contour_file() {
        if (!contours_) {
            action_text_ = L"没有可导出的等高线";
            return;
        }
        const auto file = choose_file(true, L"terrain.dcontour",
            L"DCONTOUR 等高线 (*.dcontour;*.txt)\0*.dcontour;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dcontour");
        if (file.empty()) return;
        const dt_status status = dt_contours_save_text(
            contours_, wide_to_utf8(file.c_str()).c_str());
        action_text_ = status == DT_OK ? L"已导出等高线：" + file
                                       : L"等高线导出失败：" + last_error_text();
    }

    void import_gdal_raster() {
        if (!gdal_gtiff_available_) {
            action_text_ = L"当前构建未启用 GDAL GeoTIFF 驱动";
            return;
        }
        const auto file = choose_file(
            false, L"terrain.tif",
            L"GeoTIFF / COG (*.tif;*.tiff)\0*.tif;*.tiff\0所有文件 (*.*)\0*.*\0",
            L"tif");
        if (file.empty()) return;
        dt_gdal_raster_load_options options{};
        options.struct_size = sizeof(options);
        options.band_index = 1;
        dt_grid_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_load_gdal_raster(
            wide_to_utf8(file.c_str()).c_str(), &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"GeoTIFF/COG 导入失败：" + last_error_text();
            return;
        }
        destroy_grid_layer();
        grid_ = output;
        show_grid_ = true;
        update_layer_menu();
        refresh_grid_cache();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已导入 GeoTIFF/COG：" << grid_info_.width << L"×"
             << grid_info_.height << L"，有效节点 "
             << grid_info_.valid_value_count << L"，耗时 " << std::fixed
             << std::setprecision(1) << ms << L" ms";
        if (grid_info_.width > grid_preview_width_ ||
            grid_info_.height > grid_preview_height_)
            text << L"；LOD 预览 " << grid_preview_width_ << L"×"
                 << grid_preview_height_;
        action_text_ = text.str();
    }

    void export_gdal_raster(bool cog) {
        if (!grid_) {
            action_text_ = L"没有可导出的 GRID";
            return;
        }
        if ((cog && !gdal_cog_available_) ||
            (!cog && !gdal_gtiff_available_)) {
            action_text_ = L"当前构建没有所需的 GDAL 栅格驱动";
            return;
        }
        const auto file = choose_file(
            true, cog ? L"terrain_cog.tif" : L"terrain.tif",
            L"GeoTIFF (*.tif;*.tiff)\0*.tif;*.tiff\0所有文件 (*.*)\0*.*\0",
            L"tif");
        if (file.empty()) return;
        const char* gtiff_options[] = {
            "TILED=YES", "COMPRESS=DEFLATE", "BIGTIFF=IF_SAFER", nullptr};
        const char* cog_options[] = {
            "COMPRESS=DEFLATE", "BIGTIFF=IF_SAFER", nullptr};
        dt_gdal_raster_save_options options{};
        options.struct_size = sizeof(options);
        options.driver_name = cog ? "COG" : "GTiff";
        options.creation_options = cog ? cog_options : gtiff_options;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_grid_save_gdal_raster(
            grid_, wide_to_utf8(file.c_str()).c_str(), &options);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = (cog ? L"COG 导出失败：" : L"GeoTIFF 导出失败：") +
                           last_error_text();
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << (cog ? L"已导出 COG：" : L"已导出 GeoTIFF：") << file
             << L"，耗时 " << std::fixed << std::setprecision(1) << ms
             << L" ms";
        action_text_ = text.str();
    }

    void import_gdal_contours() {
        if (!gdal_gpkg_available_) {
            action_text_ = L"当前构建未启用 GDAL GeoPackage 驱动";
            return;
        }
        const auto file = choose_file(
            false, L"terrain_contours.gpkg",
            L"GeoPackage (*.gpkg)\0*.gpkg\0所有矢量文件 (*.*)\0*.*\0",
            L"gpkg");
        if (file.empty()) return;
        dt_gdal_contour_load_options options{};
        options.struct_size = sizeof(options);
        options.elevation_field = "elevation";
        dt_contour_handle output = nullptr;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_contours_load_gdal_vector(
            wide_to_utf8(file.c_str()).c_str(), &options, &output);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"GeoPackage 等高线导入失败：" + last_error_text();
            return;
        }
        destroy_contour_layer();
        contours_ = output;
        contour_info_ = {};
        contour_info_.struct_size = sizeof(contour_info_);
        dt_contours_get_info(contours_, &contour_info_);
        show_contours_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已导入 GeoPackage 等高线：" << contour_info_.line_count
             << L" 条，" << contour_info_.vertex_count << L" 点，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void export_gdal_contours() {
        if (!contours_) {
            action_text_ = L"没有可导出的等高线";
            return;
        }
        if (!gdal_gpkg_available_) {
            action_text_ = L"当前构建未启用 GDAL GeoPackage 驱动";
            return;
        }
        const auto file = choose_file(
            true, L"terrain_contours.gpkg",
            L"GeoPackage (*.gpkg)\0*.gpkg\0所有矢量文件 (*.*)\0*.*\0",
            L"gpkg");
        if (file.empty()) return;
        const char* layer_options[] = {"SPATIAL_INDEX=YES", nullptr};
        dt_gdal_contour_save_options options{};
        options.struct_size = sizeof(options);
        options.driver_name = "GPKG";
        options.layer_name = "contours";
        options.elevation_field = "elevation";
        options.layer_creation_options = layer_options;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_contours_save_gdal_vector(
            contours_, wide_to_utf8(file.c_str()).c_str(), &options);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status != DT_OK) {
            action_text_ = L"GeoPackage 等高线导出失败：" + last_error_text();
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已导出 GeoPackage 等高线：" << file << L"，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    bool refresh_cdt_constraints() {
        cdt_triangles_.clear();
        constraint_lines_.clear();
        cdt_info_ = {};
        cdt_info_.struct_size = sizeof(cdt_info_);
        if (!cdt_ || dt_cdt_get_statistics(cdt_, &cdt_info_) != DT_OK)
            return false;
        constraint_lines_.reserve(static_cast<size_t>(cdt_info_.constraint_count));
        for (uint64_t index = 0; index < cdt_info_.constraint_count; ++index) {
            dt_constraint_info info{};
            info.struct_size = sizeof(info);
            if (dt_cdt_get_constraint_info(cdt_, index, &info) != DT_OK)
                return false;
            ConstraintLine line;
            line.id = info.id;
            line.kind = info.kind;
            line.flags = info.flags;
            line.points.resize(static_cast<size_t>(info.point_count));
            if (info.point_count != 0 &&
                dt_cdt_copy_constraint_points(cdt_, info.id, line.points.data(),
                                              info.point_count, nullptr) != DT_OK)
                return false;
            constraint_lines_.push_back(std::move(line));
        }
        return true;
    }

    void import_cdt_file() {
        const auto file = choose_file(
            false, L"terrain.dcdt",
            L"DCDT 约束网 (*.dcdt;*.txt)\0*.dcdt;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dcdt");
        if (file.empty()) return;
        set_wait_cursor(true);
        const auto begin = std::chrono::steady_clock::now();
        const dt_status status = dt_cdt_load_text(
            cdt_, wide_to_utf8(file.c_str()).c_str(), nullptr);
        const auto end = std::chrono::steady_clock::now();
        set_wait_cursor(false);
        if (status == DT_OK) clear_analysis_for_source(ProfileSource::Cdt);
        if (status != DT_OK || !refresh_cdt_constraints()) {
            action_text_ = L"约束网导入失败：" + last_error_text();
            return;
        }
        cdt_draft_.clear();
        show_cdt_ = true;
        update_layer_menu();
        enter_2d_view();
        reset_view();
        invalidate_mesh_cache();
        const double ms =
            std::chrono::duration<double, std::milli>(end - begin).count();
        std::wostringstream text;
        text << L"已打开约束网：" << cdt_info_.vertex_count << L" 顶点，"
             << cdt_info_.domain_triangle_count << L" 域内面，"
             << cdt_info_.constraint_count << L" 条约束，耗时 "
             << std::fixed << std::setprecision(1) << ms << L" ms";
        action_text_ = text.str();
    }

    void export_cdt_file() {
        if (!cdt_ || cdt_info_.vertex_count == 0) {
            action_text_ = L"没有可保存的约束网";
            return;
        }
        const auto file = choose_file(
            true, L"terrain.dcdt",
            L"DCDT 约束网 (*.dcdt;*.txt)\0*.dcdt;*.txt\0所有文件 (*.*)\0*.*\0",
            L"dcdt");
        if (file.empty()) return;
        const dt_status status =
            dt_cdt_save_text(cdt_, wide_to_utf8(file.c_str()).c_str());
        action_text_ = status == DT_OK ? L"已保存约束网：" + file
                                       : L"约束网保存失败：" + last_error_text();
    }

    void toggle_layer(int id) {
        enter_2d_view();
        if (id == ID_LAYER_TIN) show_tin_ = !show_tin_;
        else if (id == ID_LAYER_GRID) {
            if (!grid_) action_text_ = L"当前没有 GRID 图层";
            else show_grid_ = !show_grid_;
        } else if (id == ID_LAYER_CONTOURS) {
            if (!contours_) action_text_ = L"当前没有等高线图层";
            else show_contours_ = !show_contours_;
        } else if (id == ID_LAYER_CDT) {
            if (!cdt_ || cdt_info_.vertex_count == 0)
                action_text_ = L"当前没有约束 Delaunay 图层";
            else
                show_cdt_ = !show_cdt_;
        }
        update_layer_menu();
        reset_view();
        invalidate_mesh_cache();
    }

    void on_command(int id) {
        if (terrain_task_running_) {
            if (id == ID_TERRAIN_CANCEL) {
                dt_task_request_cancel(terrain_task_);
                action_text_ = L"已请求取消专题计算…";
            } else {
                action_text_ = L"专题计算正在进行；按 Esc 或选择“取消正在进行的专题计算”";
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        switch (id) {
        case ID_GENERATE_100K: generate(100000); break;
        case ID_GENERATE_1M: generate(1000000); break;
        case ID_CLEAR: clear_mesh(); break;
        case ID_MODE_INSERT:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            show_tin_ = true; update_layer_menu();
            mode_ = Mode::Insert; action_text_ = L"单击网格位置插入地形点"; break;
        case ID_MODE_DELETE:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            show_tin_ = true; update_layer_menu();
            mode_ = Mode::Delete; action_text_ = L"单击位置删除最近顶点"; break;
        case ID_MODE_QUERY:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            show_tin_ = true; update_layer_menu();
            mode_ = Mode::Query; action_text_ = L"单击查询最近顶点和覆盖三角形"; break;
        case ID_MODE_ZOOM_BOX:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            cancel_box_zoom();
            mode_ = Mode::ZoomBox;
            action_text_ = L"按住左键拖出矩形，松开后放大并适屏";
            break;
        case ID_FULL_EXTENT:
            if (view_mode_ == ViewMode::Terrain3D) {
                reset_camera3d();
                action_text_ = L"3D 相机已适屏复位";
            } else {
                reset_view();
                invalidate_mesh_cache();
                action_text_ = L"二维视图已显示全图";
            }
            break;
        case ID_IMPORT_POINTS: import_points_file(); break;
        case ID_SAVE: save_file(); break;
        case ID_LOAD: load_file(); break;
        case ID_VIEW_3D: toggle_view_mode(); break;
        case ID_Z_EXAGGERATION: change_exaggeration(1); break;
        case ID_PROFILE_MODE:
            enter_2d_view();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            cancel_box_zoom();
            mode_ = Mode::Profile;
            action_text_ = profile_complete_
                ? L"单击新的剖面起点；现有结果可先导出 CSV"
                : L"单击剖面起点，再单击终点；Esc 清除";
            break;
        case ID_PROFILE_EXPORT: export_profile_csv(); break;
        case ID_PROFILE_CLEAR:
            clear_profile();
            action_text_ = L"剖面已清除";
            break;
        case ID_MEASURE_MODE: begin_measurement_mode(); break;
        case ID_MEASURE_FINISH: finish_measurement(); break;
        case ID_MEASURE_DATUM: set_measurement_datum(); break;
        case ID_MEASURE_EXPORT: export_measurement_csv(); break;
        case ID_MEASURE_CLEAR:
            clear_measurement();
            action_text_ = L"面积/土方量测已清除";
            break;
        case ID_GRID_MASK_MEASUREMENT:
            clip_grid_with_measurement(0);
            break;
        case ID_GRID_CLIP_MEASUREMENT:
            clip_grid_with_measurement(DT_GRID_CLIP_CROP_TO_BOUNDS);
            break;
        case ID_GRID_INVERT_MEASUREMENT:
            clip_grid_with_measurement(DT_GRID_CLIP_INVERT);
            break;
        case ID_EARTHWORK_LOAD_DESIGN: load_earthwork_design(false); break;
        case ID_EARTHWORK_LOAD_DESIGN_GDAL: load_earthwork_design(true); break;
        case ID_EARTHWORK_RESAMPLE_BILINEAR:
            resample_earthwork_design(DT_GRID_RESAMPLE_BILINEAR);
            break;
        case ID_EARTHWORK_RESAMPLE_NEAREST:
            resample_earthwork_design(DT_GRID_RESAMPLE_NEAREST);
            break;
        case ID_EARTHWORK_OFFSET_DESIGN: create_offset_design_grid(); break;
        case ID_EARTHWORK_RUN: run_earthwork_analysis(); break;
        case ID_EARTHWORK_EXPORT: export_earthwork_csv(); break;
        case ID_EARTHWORK_CLEAR:
            clear_earthwork_output(true);
            action_text_ = L"设计面与双表面土方结果已清除";
            break;
        case ID_SLOPE_MODE:
            enter_2d_view();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            cancel_box_zoom();
            mode_ = Mode::Slope;
            action_text_ = L"单击地形表面查询坡度、坡向和法向；Esc 清除结果";
            break;
        case ID_SLOPE_CLEAR:
            clear_slope_analysis();
            action_text_ = L"坡度/坡向分析结果已清除";
            break;
        case ID_TERRAIN_SLOPE_GRID:
            derive_terrain_grid(DT_GRID_TERRAIN_SLOPE_DEGREES); break;
        case ID_TERRAIN_ASPECT_GRID:
            derive_terrain_grid(DT_GRID_TERRAIN_ASPECT_DEGREES); break;
        case ID_TERRAIN_HILLSHADE_GRID:
            derive_terrain_grid(DT_GRID_TERRAIN_HILLSHADE); break;
        case ID_TERRAIN_PARAMETERS: configure_terrain_parameters(); break;
        case ID_TERRAIN_CANCEL:
            action_text_ = L"当前没有正在进行的专题计算";
            break;
        case ID_TERRAIN_SHOW_ELEVATION: show_elevation_grid(); break;
        case ID_TERRAIN_EXPORT_GRID: export_terrain_grid_file(); break;
        case ID_TERRAIN_EXPORT_GEOTIFF: export_terrain_geotiff(); break;
        case ID_TIN_TO_GRID: convert_tin_to_grid(); break;
        case ID_GRID_TO_TIN: convert_grid_to_tin(); break;
        case ID_CONTOURS_FROM_TIN: generate_contours(false); break;
        case ID_CONTOURS_FROM_GRID: generate_contours(true); break;
        case ID_TIN_FROM_CONTOURS: convert_contours_to_tin(); break;
        case ID_GRID_FROM_CONTOURS: convert_contours_to_grid(); break;
        case ID_CDT_FROM_TIN: create_cdt_from_tin(); break;
        case ID_GRID_FROM_CDT: convert_cdt_to_grid(); break;
        case ID_CONTOURS_FROM_CDT: generate_cdt_contours(); break;
        case ID_IMPORT_GRID: import_grid_file(); break;
        case ID_EXPORT_GRID: export_grid_file(); break;
        case ID_VERIFY_DGRIDB: verify_dgridb_file(); break;
        case ID_COMPACT_DGTILE: compact_current_dgtile(); break;
        case ID_IMPORT_CONTOURS: import_contour_file(); break;
        case ID_EXPORT_CONTOURS: export_contour_file(); break;
        case ID_IMPORT_GDAL_RASTER: import_gdal_raster(); break;
        case ID_EXPORT_GEOTIFF: export_gdal_raster(false); break;
        case ID_EXPORT_COG: export_gdal_raster(true); break;
        case ID_IMPORT_GDAL_CONTOURS: import_gdal_contours(); break;
        case ID_EXPORT_GPKG_CONTOURS: export_gdal_contours(); break;
        case ID_IMPORT_CDT: import_cdt_file(); break;
        case ID_EXPORT_CDT: export_cdt_file(); break;
        case ID_CDT_DRAW_BREAKLINE:
            begin_cdt_draft(Mode::CdtBreakline, DT_CONSTRAINT_BREAKLINE,
                            L"断裂线");
            break;
        case ID_CDT_DRAW_OUTER:
            begin_cdt_draft(Mode::CdtOuter, DT_CONSTRAINT_OUTER_BOUNDARY,
                            L"外边界");
            break;
        case ID_CDT_DRAW_HOLE:
            begin_cdt_draft(Mode::CdtHole, DT_CONSTRAINT_HOLE_BOUNDARY,
                            L"孔洞边界");
            break;
        case ID_CDT_BATCH_SAMPLE: add_sample_breaklines_batch(); break;
        case ID_CDT_FINISH: finish_cdt_draft(); break;
        case ID_CDT_CANCEL: cancel_cdt_draft(); break;
        case ID_CDT_MOVE_VERTEX:
            enter_2d_view();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            clear_overlays();
            mode_ = Mode::CdtMoveVertex;
            show_cdt_ = true;
            update_layer_menu();
            action_text_ = L"先单击一个约束折点，再单击新位置；Esc 取消选择";
            break;
        case ID_CDT_REMOVE_VERTEX:
            enter_2d_view();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            clear_overlays();
            mode_ = Mode::CdtRemoveVertex;
            show_cdt_ = true;
            update_layer_menu();
            action_text_ = L"单击白色约束顶点以删除；共享顶点会先请求确认";
            break;
        case ID_CDT_DELETE:
            enter_2d_view();
            cdt_draft_.clear();
            reset_cdt_move_selection();
            mode_ = Mode::CdtDelete;
            show_cdt_ = true;
            update_layer_menu();
            action_text_ = L"请在彩色约束线附近单击以删除；删除外边界前需先删除孔洞";
            break;
        case ID_LAYER_TIN:
        case ID_LAYER_GRID:
        case ID_LAYER_CONTOURS:
        case ID_LAYER_CDT: toggle_layer(id); break;
        default: break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    DemoApp* app = reinterpret_cast<DemoApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<DemoApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle(window, message, wparam, lparam)
               : DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    const wchar_t* class_name = L"DterrainDemoWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class)) return 1;

    DemoApp app;
    HWND window = CreateWindowExW(
        0, class_name, L"dterrain 动态 Delaunay TIN 演示",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, instance, &app);
    if (!window) return 2;
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
