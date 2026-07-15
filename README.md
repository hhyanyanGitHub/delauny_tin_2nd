# dterrain 动态地形三角网 DLL

`dterrain` 是一个面向测绘地形建模的 C++17 动态库。它在 XY 平面上构建
Delaunay 三角网，把 Z 保存为顶点高程属性，支持批量建网、动态插入和删除、
编辑影响区、最近顶点、点定位、范围查询及保存加载。

当前版本为 `0.4.0`，提供以下入口：

- `include/dt_api.h`：推荐使用的稳定 C ABI，支持多个三角网上下文；
- `include/dt_terrain_api.h`：GRID、等高线及地形转换接口；
- `include/dt_task_api.h`：耗时转换的异步任务、进度与取消接口；
- `include/dt_gdal_api.h`：可选 GDAL 栅格/矢量数据交换接口；
- `include/dt_legacy.hpp`：题目给出的12个 C++ 接口的功能兼容层。

## 已实现能力

- 使用 CGAL EPICK 鲁棒谓词和 `Delaunay_triangulation_2`；
- 使用 `Triangulation_hierarchy_2` 加速动态点定位；
- 批量空间排序建网；
- 顶点稳定 `uint64_t` ID；
- 插入和删除的删除面、新增面、边界边、删除边及新增边结果；
- Boost R-tree 三角形范围索引，编辑时局部更新；
- 最近 XY 顶点及面/边/顶点/凸包外分类定位；
- 版本化 `.dtin` 二进制文件；
- 简单 XYZ/CSV 散点文本导入并自动构网；
- 可读的 `.dtmesh` 顶点—三角形文本打开、保存及拓扑校验；
- DLL 内存所有权明确的结果句柄；
- 自动测试、控制台示例、规模基准程序和原生 Windows GUI。
- 仿射定位的双精度 GRID、窗口读写及 `DGRID 1` 文本往返；
- TIN→GRID 三角面线性插值、GRID→TIN 普通 Delaunay 转换；
- TIN/GRID 等高线生成、线段拓扑拼接及 `DCONTOUR 1` 文本往返；
- 转换异步任务、源数据生命周期保持、进度、等待和协作取消。
- TIN、GRID、等高线 CRS WKT 元数据及转换传播；
- 可选 GeoTIFF/COG GRID 和 GeoPackage 等高线导入导出。

## v0.3 地形转换快速示例

```cpp
#include "dt_terrain_api.h"

dt_tin_to_grid_options options{};
options.struct_size = sizeof(options);
options.width = 1001;
options.height = 1001;
options.geo_transform[0] = xmin;
options.geo_transform[1] = (xmax - xmin) / 1000.0;
options.geo_transform[3] = ymin;
options.geo_transform[5] = (ymax - ymin) / 1000.0;
options.nodata_value = -9999.0;

dt_grid_handle grid = nullptr;
dt_grid_from_tin(mesh, &options, &grid);
dt_grid_save_text(grid, "terrain.dgrid");
dt_grid_destroy(grid);
```

完整说明见 [docs/TERRAIN_API.md](docs/TERRAIN_API.md)。
可运行示例位于 `examples/terrain_conversion.cpp`。

GDAL 格式交换见 [docs/GDAL_API.md](docs/GDAL_API.md)。

## 构建

依赖：CMake 3.24+、C++17 编译器、CGAL 6.x、Boost、GMP/MPFR。推荐使用
vcpkg：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic `
  -DCMAKE_BUILD_TYPE=Release `
  -DDT_BUILD_TESTS=ON `
  -DDT_BUILD_EXAMPLES=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Visual Studio x64 同样受支持，生成器可改为 Visual Studio 2022，并使用
`x64-windows` triplet。

启用可选 GDAL 交换层：

```powershell
vcpkg install --triplet x64-mingw-dynamic --x-feature=gdal
cmake -S . -B build-gdal -G "MinGW Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic `
  -DDT_WITH_GDAL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-gdal -j 4
```

`DT_WITH_GDAL=OFF` 为默认值；此时 GDAL 函数仍导出，但返回
`DT_E_UNSUPPORTED`，现有集成无需增加运行时依赖。
GDAL 构建会把依赖 DLL 和 `share/proj` 复制到输出目录；便携分发时应保持这些
文件与目录结构。

启用规模基准：

```powershell
cmake -S . -B build -DDT_BUILD_BENCHMARKS=ON
cmake --build build -j 4
./build/dterrain_benchmark.exe 1000000
```

## GUI 演示

Windows 构建默认生成 `dterrain_demo.exe`。运行时将它与 `dterrain.dll`、
`libgmp-10.dll` 和 `libwinpthread-1.dll` 放在同一目录：

```powershell
./build/dterrain_demo.exe
```

GUI 支持：

- 一键生成10万或100万模拟地形特征点；
- 按高程分级绘制当前视口三角网；
- 鼠标滚轮缩放，鼠标右键或中键拖动平移，框选范围后自动放大适屏；
- 查询模式下显示最近顶点和覆盖三角形；
- 插入模式显示红色旧面、黄色边界和绿色新增面/边；
- 删除模式删除点击位置的最近顶点并显示局部影响；
- 导入 `.xyz/.txt/.csv` 散点并自动构网；
- 打开或保存 `.dtmesh/.txt` 文本三角网，兼容 `.dtin` 二进制网格；
- 全图复位及清空；
- 状态栏显示顶点数、三角形数、范围查询及编辑耗时。

详见 [docs/GUI.md](docs/GUI.md)。

安装或便携目录中的 `sample_data/sample_points.xyz` 可用于快速验证散点文本导入。

## 已测性能

测试环境为本项目当前 Windows x64 Release/MinGW 构建，随机均匀 XY 数据：

| 点数 | 有限三角形 | 构建时间 | 峰值工作集 | 完整校验 |
|---:|---:|---:|---:|---:|
| 100,000 | 199,973 | 0.21 s | 未单独记录 | 0.02 s |
| 1,000,000 | 1,999,962 | 3.47 s | 460.5 MB | 0.36 s |
| 10,000,000 | 19,999,951 | 50.44 s | 4,553.2 MB | 3.39 s |

这些数据用于验证数量级，不代表所有坐标分布和硬件上的固定承诺。

## 重要语义

- Delaunay 判定只使用 XY；查询参数中的 Z 不参与最近距离或点定位；
- 相同 XY 的两个点会返回 `DT_E_DUPLICATE_XY`；
- 修改高程使用 `dt_update_vertex_z()`，不会改变拓扑；
- 范围查询返回与闭合矩形相交的有限三角形；
- 旧接口返回的 `double*` 由 DLL 管理，只在同线程下一次旧接口调用前有效；
- 新接口结果必须通过对应的 `dt_release_*` 函数释放。
- GRID→TIN 遇到 NoData 默认拒绝；显式允许桥接会跨空洞构网，正式孔洞语义将在
  CDT 后端完成；
- GDAL 栅格采用像元中心与本库 GRID 节点对应；导入导出会自动进行半像元偏移；
- GeoTIFF/COG 与 GeoPackage 仅在 `DT_WITH_GDAL=ON` 构建中可用。

详细接口见 [docs/API.md](docs/API.md)，内部设计与限制见
[docs/DESIGN.md](docs/DESIGN.md)。

## Word 手册

- [dterrain DLL 开发与使用手册](docs/manuals/dterrain_DLL开发使用手册.docx)
- [dterrain GUI 操作手册与入门教程](docs/manuals/dterrain_GUI操作入门教程.docx)

手册生成脚本位于 `tools/build_manuals.py`，便于接口或 GUI 更新后同步维护文档。

## 许可证说明

本项目当前后端使用 CGAL 2D Triangulations 包。该包为 GPL 或商业许可双重模式；
本工程定位为研究和演示用途。分发或改变用途前应重新检查 CGAL 及其依赖的许可。
