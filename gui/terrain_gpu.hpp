#pragma once

#include "dt_api.h"
#include "terrain_3d.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dterrain::viewer3d {

struct GpuRenderStatistics {
    uint64_t total_chunks = 0;
    uint64_t visible_chunks = 0;
    uint64_t total_triangles = 0;
    uint64_t rendered_triangles = 0;
};

class TerrainGpuRenderer {
public:
    TerrainGpuRenderer();
    ~TerrainGpuRenderer();
    TerrainGpuRenderer(const TerrainGpuRenderer&) = delete;
    TerrainGpuRenderer& operator=(const TerrainGpuRenderer&) = delete;

    bool initialize(HWND window, std::string& error);
    void shutdown();
    bool resize(uint32_t width, uint32_t height, std::string& error);
    bool set_mesh(const std::vector<dt_triangle3>& triangles,
                  double center_x, double center_y, double center_z,
                  double xy_scale, double zmin, double zmax,
                  double z_exaggeration, std::string& error);
    void clear_mesh();
    bool render(const Camera& camera, std::string& error);
    bool pick(uint32_t x, uint32_t y, const Camera& camera,
              uint64_t& triangle_index, std::string& error);
    void select_triangle(uint64_t triangle_index);

    bool ready() const { return device_ != nullptr; }
    const GpuRenderStatistics& statistics() const { return statistics_; }
    const wchar_t* adapter_name() const { return adapter_name_.c_str(); }

private:
    struct Chunk;
    struct Vertex;
    struct Constants;

    bool create_device(std::string& error);
    bool create_pipeline(std::string& error);
    bool create_size_resources(uint32_t width, uint32_t height,
                               std::string& error);
    bool draw_scene(const Camera& camera, bool picking, std::string& error);
    void release_size_resources();
    void release_pipeline();

    HWND window_ = nullptr;
    HMODULE compiler_module_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::wstring adapter_name_ = L"Direct3D 11";
    std::vector<Chunk> chunks_;
    GpuRenderStatistics statistics_{};
    uint32_t selected_triangle_id_ = 0;

    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11RenderTargetView* render_target_ = nullptr;
    ID3D11Texture2D* depth_texture_ = nullptr;
    ID3D11DepthStencilView* depth_view_ = nullptr;
    ID3D11Texture2D* picking_texture_ = nullptr;
    ID3D11RenderTargetView* picking_target_ = nullptr;
    ID3D11Texture2D* picking_staging_ = nullptr;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11PixelShader* picking_shader_ = nullptr;
    ID3D11InputLayout* input_layout_ = nullptr;
    ID3D11Buffer* constant_buffer_ = nullptr;
    ID3D11RasterizerState* rasterizer_state_ = nullptr;
    ID3D11DepthStencilState* depth_state_ = nullptr;
};

} // namespace dterrain::viewer3d
