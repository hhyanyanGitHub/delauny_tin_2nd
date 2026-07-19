#include "terrain_gpu.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace dterrain::viewer3d {

namespace {

template <class T>
void release_com(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

std::string hresult_text(const char* operation, HRESULT result) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT=0x" << std::hex
           << static_cast<unsigned long>(result) << ')';
    return stream.str();
}

constexpr const char* kShaderSource = R"hlsl(
cbuffer CameraConstants : register(b0) {
    float4 camera_position;
    float4 camera_right;
    float4 camera_up;
    float4 camera_forward;
    float4 projection;
    uint selected_triangle_id;
    float3 constant_padding;
};

struct VSInput {
    float3 position : POSITION;
    float elevation : TEXCOORD0;
    float3 normal : NORMAL;
    uint triangle_id : BLENDINDICES0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float elevation : TEXCOORD0;
    float lighting : TEXCOORD1;
    nointerpolation uint triangle_id : TEXCOORD2;
};

VSOutput vs_main(VSInput input) {
    VSOutput output;
    float3 relative = input.position - camera_position.xyz;
    float depth = dot(relative, camera_forward.xyz);
    float near_plane = projection.z;
    float far_plane = projection.w;
    output.position = float4(
        dot(relative, camera_right.xyz) / projection.x,
        dot(relative, camera_up.xyz) / projection.y,
        far_plane / (far_plane - near_plane) * depth -
            near_plane * far_plane / (far_plane - near_plane),
        depth);
    output.elevation = input.elevation;
    output.lighting = 0.56 + 0.44 * abs(dot(input.normal,
        normalize(float3(-0.35, -0.45, 0.82))));
    output.triangle_id = input.triangle_id;
    return output;
}

float3 terrain_color(float value) {
    value = saturate(value);
    float3 low = float3(0.11, 0.31, 0.48);
    float3 middle = float3(0.30, 0.62, 0.36);
    float3 high = float3(0.88, 0.85, 0.68);
    return value < 0.5 ? lerp(low, middle, value * 2.0)
                       : lerp(middle, high, (value - 0.5) * 2.0);
}

float4 ps_main(VSOutput input) : SV_TARGET {
    float3 color = terrain_color(input.elevation) * input.lighting;
    if (input.triangle_id == selected_triangle_id)
        color = float3(1.0, 0.28, 0.04);
    return float4(color, 1.0);
}

uint ps_pick(VSOutput input) : SV_TARGET {
    return input.triangle_id;
}
)hlsl";

using CompileFunction = decltype(&D3DCompile);

bool compile_shader(CompileFunction compile, const char* entry,
                    const char* target,
                    ID3DBlob** bytecode, std::string& error) {
    ID3DBlob* messages = nullptr;
    const HRESULT result = compile(
        kShaderSource, std::strlen(kShaderSource), "dterrain_gpu.hlsl",
        nullptr, nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
        bytecode, &messages);
    if (FAILED(result)) {
        if (messages && messages->GetBufferPointer()) {
            error.assign(static_cast<const char*>(messages->GetBufferPointer()),
                         messages->GetBufferSize());
        } else {
            error = hresult_text("D3DCompile", result);
        }
        release_com(messages);
        return false;
    }
    release_com(messages);
    return true;
}

} // namespace

struct TerrainGpuRenderer::Vertex {
    float position[3]{};
    float elevation = 0.0f;
    float normal[3]{};
    uint32_t triangle_id = 0;
};

struct TerrainGpuRenderer::Constants {
    float camera_position[4]{};
    float camera_right[4]{};
    float camera_up[4]{};
    float camera_forward[4]{};
    float projection[4]{};
    uint32_t selected_triangle_id = 0;
    float padding[3]{};
};

struct TerrainGpuRenderer::Chunk {
    ID3D11Buffer* vertices = nullptr;
    uint32_t vertex_count = 0;
    uint64_t first_triangle = 0;
    Sphere bounds{};

    Chunk() = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&& other) noexcept
        : vertices(other.vertices), vertex_count(other.vertex_count),
          first_triangle(other.first_triangle), bounds(other.bounds) {
        other.vertices = nullptr;
    }
    Chunk& operator=(Chunk&& other) noexcept {
        if (this == &other) return *this;
        release_com(vertices);
        vertices = other.vertices;
        vertex_count = other.vertex_count;
        first_triangle = other.first_triangle;
        bounds = other.bounds;
        other.vertices = nullptr;
        return *this;
    }
    ~Chunk() { release_com(vertices); }
};

TerrainGpuRenderer::TerrainGpuRenderer() = default;
TerrainGpuRenderer::~TerrainGpuRenderer() { shutdown(); }

bool TerrainGpuRenderer::initialize(HWND window, std::string& error) {
    if (ready() && window_ == window) return true;
    shutdown();
    if (!window) {
        error = "GPU canvas window is null";
        return false;
    }
    window_ = window;
    if (!create_device(error) || !create_pipeline(error)) {
        shutdown();
        return false;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    if (!resize(static_cast<uint32_t>(std::max(1L, rect.right)),
                static_cast<uint32_t>(std::max(1L, rect.bottom)), error)) {
        shutdown();
        return false;
    }
    return true;
}

void TerrainGpuRenderer::shutdown() {
    if (context_) context_->ClearState();
    clear_mesh();
    release_size_resources();
    release_pipeline();
    release_com(context_);
    release_com(device_);
    release_com(swap_chain_);
    if (compiler_module_) FreeLibrary(compiler_module_);
    compiler_module_ = nullptr;
    window_ = nullptr;
    width_ = height_ = 0;
}

bool TerrainGpuRenderer::create_device(std::string& error) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window_;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const std::array<D3D_FEATURE_LEVEL, 3> levels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &description,
        &swap_chain_, &device_, &selected, &context_);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
            static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &description,
            &swap_chain_, &device_, &selected, &context_);
    }
    if (FAILED(result)) {
        error = hresult_text("D3D11CreateDeviceAndSwapChain", result);
        return false;
    }
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                          reinterpret_cast<void**>(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapter_description{};
        if (SUCCEEDED(adapter->GetDesc(&adapter_description)))
            adapter_name_ = adapter_description.Description;
    }
    release_com(adapter);
    release_com(dxgi_device);
    return true;
}

bool TerrainGpuRenderer::create_pipeline(std::string& error) {
    constexpr std::array<const wchar_t*, 3> compiler_names{
        L"d3dcompiler_47.dll", L"d3dcompiler_46.dll",
        L"d3dcompiler_43.dll"};
    for (const wchar_t* name : compiler_names) {
        compiler_module_ = LoadLibraryW(name);
        if (compiler_module_) break;
    }
    if (!compiler_module_) {
        error = "Direct3D shader compiler DLL is unavailable";
        return false;
    }
    const auto compile = reinterpret_cast<CompileFunction>(
        GetProcAddress(compiler_module_, "D3DCompile"));
    if (!compile) {
        error = "D3DCompile entry point is unavailable";
        return false;
    }
    ID3DBlob* vertex_bytecode = nullptr;
    ID3DBlob* pixel_bytecode = nullptr;
    ID3DBlob* picking_bytecode = nullptr;
    if (!compile_shader(compile, "vs_main", "vs_4_0", &vertex_bytecode,
                        error) ||
        !compile_shader(compile, "ps_main", "ps_4_0", &pixel_bytecode,
                        error) ||
        !compile_shader(compile, "ps_pick", "ps_4_0", &picking_bytecode,
                        error)) {
        release_com(vertex_bytecode);
        release_com(pixel_bytecode);
        release_com(picking_bytecode);
        return false;
    }
    HRESULT result = device_->CreateVertexShader(
        vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize(),
        nullptr, &vertex_shader_);
    if (SUCCEEDED(result))
        result = device_->CreatePixelShader(
            pixel_bytecode->GetBufferPointer(), pixel_bytecode->GetBufferSize(),
            nullptr, &pixel_shader_);
    if (SUCCEEDED(result))
        result = device_->CreatePixelShader(
            picking_bytecode->GetBufferPointer(),
            picking_bytecode->GetBufferSize(), nullptr, &picking_shader_);
    const D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0, 28,
         D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (SUCCEEDED(result))
        result = device_->CreateInputLayout(
            elements, static_cast<UINT>(std::size(elements)),
            vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize(),
            &input_layout_);
    release_com(vertex_bytecode);
    release_com(pixel_bytecode);
    release_com(picking_bytecode);
    if (FAILED(result)) {
        error = hresult_text("create Direct3D shaders/input layout", result);
        return false;
    }

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(Constants);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(&constants, nullptr, &constant_buffer_);

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (SUCCEEDED(result))
        result = device_->CreateRasterizerState(&rasterizer, &rasterizer_state_);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS;
    if (SUCCEEDED(result))
        result = device_->CreateDepthStencilState(&depth, &depth_state_);
    if (FAILED(result)) {
        error = hresult_text("create Direct3D pipeline state", result);
        return false;
    }
    return true;
}

void TerrainGpuRenderer::release_pipeline() {
    release_com(depth_state_);
    release_com(rasterizer_state_);
    release_com(constant_buffer_);
    release_com(input_layout_);
    release_com(picking_shader_);
    release_com(pixel_shader_);
    release_com(vertex_shader_);
}

void TerrainGpuRenderer::release_size_resources() {
    release_com(picking_staging_);
    release_com(picking_target_);
    release_com(picking_texture_);
    release_com(depth_view_);
    release_com(depth_texture_);
    release_com(render_target_);
}

bool TerrainGpuRenderer::resize(uint32_t width, uint32_t height,
                                std::string& error) {
    if (!ready()) {
        error = "Direct3D renderer is not initialized";
        return false;
    }
    width = std::max(1U, width);
    height = std::max(1U, height);
    if (width == width_ && height == height_ && render_target_) return true;
    release_size_resources();
    HRESULT result = swap_chain_->ResizeBuffers(0, width, height,
                                                 DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        error = hresult_text("IDXGISwapChain::ResizeBuffers", result);
        return false;
    }
    width_ = width;
    height_ = height;
    return create_size_resources(width, height, error);
}

bool TerrainGpuRenderer::create_size_resources(uint32_t width, uint32_t height,
                                                std::string& error) {
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT result = swap_chain_->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (SUCCEEDED(result))
        result = device_->CreateRenderTargetView(back_buffer, nullptr,
                                                  &render_target_);
    release_com(back_buffer);

    D3D11_TEXTURE2D_DESC depth_description{};
    depth_description.Width = width;
    depth_description.Height = height;
    depth_description.MipLevels = 1;
    depth_description.ArraySize = 1;
    depth_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_description.SampleDesc.Count = 1;
    depth_description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(result))
        result = device_->CreateTexture2D(&depth_description, nullptr,
                                           &depth_texture_);
    if (SUCCEEDED(result))
        result = device_->CreateDepthStencilView(depth_texture_, nullptr,
                                                  &depth_view_);

    D3D11_TEXTURE2D_DESC picking_description{};
    picking_description.Width = width;
    picking_description.Height = height;
    picking_description.MipLevels = 1;
    picking_description.ArraySize = 1;
    picking_description.Format = DXGI_FORMAT_R32_UINT;
    picking_description.SampleDesc.Count = 1;
    picking_description.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(result))
        result = device_->CreateTexture2D(&picking_description, nullptr,
                                           &picking_texture_);
    if (SUCCEEDED(result))
        result = device_->CreateRenderTargetView(picking_texture_, nullptr,
                                                  &picking_target_);
    picking_description.Width = 1;
    picking_description.Height = 1;
    picking_description.BindFlags = 0;
    picking_description.Usage = D3D11_USAGE_STAGING;
    picking_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(result))
        result = device_->CreateTexture2D(&picking_description, nullptr,
                                           &picking_staging_);
    if (FAILED(result)) {
        error = hresult_text("create Direct3D size-dependent resources", result);
        release_size_resources();
        return false;
    }
    return true;
}

void TerrainGpuRenderer::clear_mesh() {
    chunks_.clear();
    statistics_ = {};
    selected_triangle_id_ = 0;
}

void TerrainGpuRenderer::select_triangle(uint64_t triangle_index) {
    selected_triangle_id_ = triangle_index <
                                    std::numeric_limits<uint32_t>::max() - 1ULL
                                ? static_cast<uint32_t>(triangle_index + 1ULL)
                                : 0U;
}

bool TerrainGpuRenderer::set_mesh(
    const std::vector<dt_triangle3>& triangles, double center_x,
    double center_y, double center_z, double xy_scale, double zmin,
    double zmax, double z_exaggeration, std::string& error) {
    clear_mesh();
    if (!ready()) {
        error = "Direct3D renderer is not initialized";
        return false;
    }
    if (triangles.size() >= std::numeric_limits<uint32_t>::max()) {
        error = "GPU picking supports at most UINT32_MAX-1 triangles";
        return false;
    }
    xy_scale = std::max(xy_scale, 1.0e-12);
    const double z_range = std::max(zmax - zmin, 1.0e-12);
    constexpr size_t kChunkTriangles = 16384;
    const auto ranges = make_chunk_ranges(triangles.size(), kChunkTriangles);
    chunks_.reserve(ranges.size());
    for (const ChunkRange& range : ranges) {
        std::vector<Vertex> vertices(range.triangle_count * 3);
        Vec3 minimum{std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max()};
        Vec3 maximum{std::numeric_limits<double>::lowest(),
                     std::numeric_limits<double>::lowest(),
                     std::numeric_limits<double>::lowest()};
        for (size_t local = 0; local < range.triangle_count; ++local) {
            const size_t triangle_index = range.first_triangle + local;
            const auto& triangle = triangles[triangle_index];
            std::array<Vec3, 3> positions{};
            for (size_t vertex = 0; vertex < 3; ++vertex) {
                const auto& point = triangle.vertex[vertex].point;
                positions[vertex] = {
                    (point.x - center_x) / xy_scale,
                    (point.y - center_y) / xy_scale,
                    (point.z - center_z) / xy_scale * z_exaggeration};
                minimum.x = std::min(minimum.x, positions[vertex].x);
                minimum.y = std::min(minimum.y, positions[vertex].y);
                minimum.z = std::min(minimum.z, positions[vertex].z);
                maximum.x = std::max(maximum.x, positions[vertex].x);
                maximum.y = std::max(maximum.y, positions[vertex].y);
                maximum.z = std::max(maximum.z, positions[vertex].z);
            }
            const Vec3 normal = normalized(cross(positions[1] - positions[0],
                                                  positions[2] - positions[0]));
            for (size_t vertex = 0; vertex < 3; ++vertex) {
                Vertex& output = vertices[local * 3 + vertex];
                output.position[0] = static_cast<float>(positions[vertex].x);
                output.position[1] = static_cast<float>(positions[vertex].y);
                output.position[2] = static_cast<float>(positions[vertex].z);
                output.elevation = static_cast<float>(std::clamp(
                    (triangle.vertex[vertex].point.z - zmin) / z_range,
                    0.0, 1.0));
                output.normal[0] = static_cast<float>(normal.x);
                output.normal[1] = static_cast<float>(normal.y);
                output.normal[2] = static_cast<float>(normal.z);
                output.triangle_id = static_cast<uint32_t>(triangle_index + 1);
            }
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = vertices.data();
        Chunk chunk{};
        const HRESULT result = device_->CreateBuffer(&description, &initial,
                                                      &chunk.vertices);
        if (FAILED(result)) {
            error = hresult_text("upload terrain GPU chunk", result);
            clear_mesh();
            return false;
        }
        chunk.vertex_count = static_cast<uint32_t>(vertices.size());
        chunk.first_triangle = range.first_triangle;
        chunk.bounds.center = (minimum + maximum) * 0.5;
        chunk.bounds.radius = length(maximum - chunk.bounds.center);
        chunks_.push_back(std::move(chunk));
    }
    statistics_.total_chunks = chunks_.size();
    statistics_.total_triangles = triangles.size();
    return true;
}

bool TerrainGpuRenderer::draw_scene(const Camera& camera, bool picking,
                                    std::string& error) {
    if (!render_target_ || !depth_view_) {
        error = "Direct3D render targets are unavailable";
        return false;
    }
    const Basis basis = camera_basis(camera);
    Constants constants{};
    const auto assign = [](float (&output)[4], Vec3 input) {
        output[0] = static_cast<float>(input.x);
        output[1] = static_cast<float>(input.y);
        output[2] = static_cast<float>(input.z);
    };
    assign(constants.camera_position, basis.position);
    assign(constants.camera_right, basis.right);
    assign(constants.camera_up, basis.up);
    assign(constants.camera_forward, basis.forward);
    const double aspect = static_cast<double>(width_) /
                          static_cast<double>(std::max(1U, height_));
    constants.projection[0] = static_cast<float>(
        std::tan(camera.fov_y * 0.5) * aspect);
    constants.projection[1] = static_cast<float>(std::tan(camera.fov_y * 0.5));
    constants.projection[2] = 0.02f;
    constants.projection[3] = 80.0f;
    constants.selected_triangle_id = selected_triangle_id_;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = context_->Map(constant_buffer_, 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) {
        error = hresult_text("map Direct3D camera constants", result);
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constant_buffer_, 0);

    ID3D11RenderTargetView* target = picking ? picking_target_ : render_target_;
    const float background[4]{0.055f, 0.075f, 0.09f, 1.0f};
    if (picking) {
        const UINT clear[4]{0, 0, 0, 0};
        context_->ClearRenderTargetView(target,
            reinterpret_cast<const float*>(clear));
    } else {
        context_->ClearRenderTargetView(target, background);
    }
    context_->ClearDepthStencilView(depth_view_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    context_->OMSetRenderTargets(1, &target, depth_view_);
    context_->OMSetDepthStencilState(depth_state_, 0);
    context_->RSSetState(rasterizer_state_);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(input_layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constant_buffer_);
    context_->PSSetShader(picking ? picking_shader_ : pixel_shader_, nullptr, 0);
    statistics_.visible_chunks = 0;
    statistics_.rendered_triangles = 0;
    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    for (const Chunk& chunk : chunks_) {
        if (!sphere_in_view(chunk.bounds, camera, aspect, 0.02, 80.0))
            continue;
        context_->IASetVertexBuffers(0, 1, &chunk.vertices, &stride, &offset);
        context_->Draw(chunk.vertex_count, 0);
        ++statistics_.visible_chunks;
        statistics_.rendered_triangles += chunk.vertex_count / 3;
    }
    return true;
}

bool TerrainGpuRenderer::render(const Camera& camera, std::string& error) {
    if (!draw_scene(camera, false, error)) return false;
    const HRESULT result = swap_chain_->Present(1, 0);
    if (FAILED(result)) {
        error = hresult_text("IDXGISwapChain::Present", result);
        return false;
    }
    return true;
}

bool TerrainGpuRenderer::pick(uint32_t x, uint32_t y, const Camera& camera,
                              uint64_t& triangle_index,
                              std::string& error) {
    triangle_index = std::numeric_limits<uint64_t>::max();
    if (x >= width_ || y >= height_) return false;
    if (!draw_scene(camera, true, error)) return false;
    const D3D11_BOX source{x, y, 0, x + 1, y + 1, 1};
    context_->CopySubresourceRegion(picking_staging_, 0, 0, 0, 0,
                                    picking_texture_, 0, &source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context_->Map(picking_staging_, 0,
                                         D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        error = hresult_text("read GPU picking result", result);
        return false;
    }
    const uint32_t identifier = *static_cast<const uint32_t*>(mapped.pData);
    context_->Unmap(picking_staging_, 0);
    if (identifier == 0) return false;
    triangle_index = static_cast<uint64_t>(identifier - 1);
    return true;
}

} // namespace dterrain::viewer3d
