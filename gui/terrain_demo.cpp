#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include "dt_api.h"
#include "terrain_3d.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
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
    ID_Z_EXAGGERATION
};

enum class Mode { Insert, Delete, Query, ZoomBox };
enum class ViewMode { Map2D, Terrain3D };
enum class Drag3D { None, Orbit, Pan };

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
    DemoApp() { dt_create(nullptr, &mesh_); }
    ~DemoApp() { dt_destroy(mesh_); }

    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        hwnd_ = window;
        switch (message) {
        case WM_CREATE:
            create_controls();
            generate(100000);
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wparam));
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
            panning_ = false;
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
            if (view_mode_ == ViewMode::Terrain3D && handle_key3d(wparam)) return 0;
            if (wparam == VK_ESCAPE && box_zooming_) {
                cancel_box_zoom();
                action_text_ = L"已取消框选放大";
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
    Mode mode_ = Mode::Query;
    ViewMode view_mode_ = ViewMode::Map2D;
    dt_bounds2 view_{0.0, 0.0, 1.0, 1.0};
    bool view_valid_ = false;
    bool cache_valid_ = false;
    std::vector<dt_triangle3> triangles_;
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
    }

    void reset_view() {
        dt_statistics stats{};
        if (dt_get_statistics(mesh_, &stats) != DT_OK || stats.vertex_count == 0) {
            view_valid_ = false;
            return;
        }
        fit_view_to_bounds(stats.bounds, 1.08);
        camera_needs_reset_ = true;
    }

    void invalidate_mesh_cache() {
        cache_valid_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        clear_overlays();
        if (!effect) return;
        dt_edit_result_view view{};
        if (dt_edit_result_get_view(effect, &view) != DT_OK) return;
        if (view.removed_triangle_count)
            removed_triangles_.assign(view.removed_triangles,
                                      view.removed_triangles + view.removed_triangle_count);
        if (view.added_triangle_count)
            added_triangles_.assign(view.added_triangles,
                                    view.added_triangles + view.added_triangle_count);
        if (view.boundary_edge_count)
            boundary_edges_.assign(view.boundary_edges,
                                   view.boundary_edges + view.boundary_edge_count);
        if (view.added_edge_count)
            added_edges_.assign(view.added_edges,
                                view.added_edges + view.added_edge_count);
    }

    void on_left_click(int x, int y) {
        if (!view_valid_) return;
        const dt_point3 world = screen_to_world(x, y);
        const auto begin = std::chrono::steady_clock::now();
        if (mode_ == Mode::Insert) {
            dt_point3 point{world.x, world.y, terrain_z(world.x, world.y)};
            dt_vertex_id id = 0;
            dt_edit_result effect = nullptr;
            const auto status = dt_insert_point(mesh_, &point, &id, &effect);
            if (status == DT_OK) {
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

    void zoom(int x, int y, int delta) {
        if (!view_valid_ || y < kToolbarHeight) return;
        const auto anchor = screen_to_world(x, y);
        const double factor = delta > 0 ? 0.78 : 1.28;
        view_.xmin = anchor.x + (view_.xmin - anchor.x) * factor;
        view_.xmax = anchor.x + (view_.xmax - anchor.x) * factor;
        view_.ymin = anchor.y + (view_.ymin - anchor.y) * factor;
        view_.ymax = anchor.y + (view_.ymax - anchor.y) * factor;
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
        dt_query_result result = nullptr;
        if (dt_query_triangles(mesh_, &view_, &result) != DT_OK) {
            action_text_ = L"范围查询失败：" + last_error_text();
            return;
        }
        dt_query_result_view view{};
        if (dt_query_result_get_view(result, &view) == DT_OK) {
            if (view.triangle_count)
                triangles_.assign(view.triangles, view.triangles + view.triangle_count);
            else
                triangles_.clear();
            cache_valid_ = true;
        }
        dt_release_query_result(result);
        const auto end = std::chrono::steady_clock::now();
        last_query_ms_ = std::chrono::duration<double, std::milli>(end - begin).count();
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
            draw_mesh(dc);
            draw_triangle_set(dc, removed_triangles_, RGB(245, 68, 70), 2);
            draw_segment_set(dc, boundary_edges_, RGB(255, 210, 50), 3);
            draw_triangle_set(dc, added_triangles_, RGB(60, 220, 100), 2);
            draw_segment_set(dc, added_edges_, RGB(50, 255, 130), 3);
            draw_highlight(dc);
            draw_box_zoom(dc);
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
                              mode_ == Mode::ZoomBox ? L"框选放大" : L"查询";
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
             << L" | "
             << action_text_;
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
            clear_overlays();
            reset_view();
            action_text_ = L"已加载：" + file;
            invalidate_mesh_cache();
        } else {
            action_text_ = L"加载失败：" + last_error_text();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void on_command(int id) {
        switch (id) {
        case ID_GENERATE_100K: generate(100000); break;
        case ID_GENERATE_1M: generate(1000000); break;
        case ID_CLEAR: clear_mesh(); break;
        case ID_MODE_INSERT:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            mode_ = Mode::Insert; action_text_ = L"单击网格位置插入地形点"; break;
        case ID_MODE_DELETE:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            mode_ = Mode::Delete; action_text_ = L"单击位置删除最近顶点"; break;
        case ID_MODE_QUERY:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
            mode_ = Mode::Query; action_text_ = L"单击查询最近顶点和覆盖三角形"; break;
        case ID_MODE_ZOOM_BOX:
            if (view_mode_ == ViewMode::Terrain3D) toggle_view_mode();
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
