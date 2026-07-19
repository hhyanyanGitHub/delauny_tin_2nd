#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "terrain_gpu.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

dt_triangle3 sample_triangle() {
    dt_triangle3 triangle{};
    triangle.vertex[0].id = 1;
    triangle.vertex[0].point = {-1.0, -1.0, 0.0};
    triangle.vertex[1].id = 2;
    triangle.vertex[1].point = {1.0, -1.0, 0.0};
    triangle.vertex[2].id = 3;
    triangle.vertex[2].point = {0.0, 1.0, 0.0};
    return triangle;
}

} // namespace

int main() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"DterrainGpuRendererTest";
    assert(RegisterClassExW(&window_class) != 0);
    HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
    assert(window != nullptr);

    dterrain::viewer3d::TerrainGpuRenderer renderer;
    std::string error;
    assert(renderer.initialize(window, error));
    const std::vector<dt_triangle3> triangles{sample_triangle()};
    assert(renderer.set_mesh(triangles, 0.0, 0.0, 0.0, 1.0,
                             0.0, 1.0, 1.0, error));
    dterrain::viewer3d::Camera camera{};
    assert(renderer.render(camera, error));
    uint64_t picked = 999;
    assert(renderer.pick(320, 240, camera, picked, error));
    assert(picked == 0);
    assert(renderer.statistics().total_chunks == 1);
    assert(renderer.statistics().rendered_triangles == 1);
    renderer.shutdown();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    std::cout << "GPU renderer tests passed\n";
    return 0;
}
