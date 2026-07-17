# dterrain 动态地形三角网 DLL

`dterrain` 是一个面向测绘地形建模的 C++17 动态库。它在 XY 平面上构建
Delaunay 三角网，把 Z 保存为顶点高程属性，支持批量建网、动态插入和删除、
编辑影响区、最近顶点、点定位、范围查询及保存加载。

当前版本为 `0.13.0`，提供以下入口：

- `include/dt_api.h`：推荐使用的稳定 C ABI，支持多个三角网上下文；
- `include/dt_cdt_api.h`：折线约束、外边界、孔洞及域内三角形接口；
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
- 自动测试、控制台示例、规模基准程序和原生 Windows 2D/3D GUI；
- 仿射定位的双精度 GRID、窗口读写及 `DGRID 1` 文本往返；
- TIN→GRID 三角面线性插值、GRID→TIN 普通 Delaunay 转换；
- TIN/GRID 等高线生成、线段拓扑拼接及 `DCONTOUR 1` 文本往返；
- 等高线折点或加密样本反向重建普通 TIN，并可直接插值为 GRID；
- 转换异步任务、源数据生命周期保持、进度、等待和协作取消。
- TIN、GRID、等高线 CRS WKT 元数据及转换传播；
- 可选 GeoTIFF/COG GRID 和 GeoPackage 等高线导入导出；启用 GDAL 的 GUI
  可直接完成这些格式的打开、保存与图层叠加。
- 独立约束 Delaunay 句柄，支持断裂线、外边界、孔洞、约束增删、域内查询及
  `DCDT 1` 文本往返。
- 普通 TIN→CDT 初始化、CDT 域内高程采样，以及尊重外边界和孔洞的
  CDT→GRID/等高线转换。
- 保持稳定约束 ID 的原子几何更新，以及可选的新旧域网格影响结果。
- 约束顶点引用查询、共享顶点默认保护，以及显式的单约束脱离删除。
- 原子批量约束事务，一次提交多条新增、更新和删除并只重建一次候选 CDT。

## v0.13 GUI GDAL 数据交换

以 `DT_WITH_GDAL=ON` 构建演示程序后，“数据交换”菜单提供 GeoTIFF/COG GRID
和 GeoPackage 等高线的导入导出。程序启动时探测 `GTiff`、`COG`、`GPKG` 驱动，
缺少相应驱动时只将对应菜单置灰，不影响普通 TIN、文本格式和动态编辑功能。

导入栅格只替换 GRID 图层，导入 GeoPackage 只替换等高线图层；已有 TIN、CDT
和另一类派生图层保持不变，便于多源数据叠加检查。GeoTIFF 默认使用分块、DEFLATE
压缩和 `BIGTIFF=IF_SAFER`，COG 使用 DEFLATE 与 `BIGTIFF=IF_SAFER`。

## v0.12 等高线反向转换快速示例

```cpp
#include "dt_terrain_api.h"

dt_contours_to_tin_options sampling{};
sampling.struct_size = sizeof(sampling);
sampling.maximum_segment_length = 5.0; // 0 表示只使用原折点
sampling.merge_tolerance = 1.0e-8;      // 0 表示只合并完全相同 XY

dt_tin_from_contours(contours, &sampling, mesh);

dt_grid_handle grid = nullptr;
dt_grid_from_contours(contours, &sampling, &grid_options, &grid);
dt_grid_destroy(grid);
```

`LINE elevation` 是转换时的权威 Z。相同或容差内 XY 的高程冲突会拒绝整次转换，
原输出 TIN 保持不变。生成的是普通 Delaunay TIN，不保证原等高线折线成为网边；
需要硬折线约束时应使用独立 CDT。

## v0.11 约束 Delaunay 快速示例

```cpp
#include "dt_cdt_api.h"

dt_cdt_handle cdt = nullptr;
dt_cdt_create(nullptr, &cdt);
dt_cdt_build_from_tin(cdt, tin);

dt_constraint_id boundary_id = 0;
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_OUTER_BOUNDARY, 0,
                      boundary, boundary_point_count, &boundary_id);
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_HOLE_BOUNDARY, 0,
                      hole, hole_point_count, nullptr);

dt_edit_result effect = nullptr;
dt_cdt_update_constraint(cdt, boundary_id, DT_CONSTRAINT_CLOSED,
                         moved_boundary, boundary_point_count, &effect);
// boundary_id 保持不变；effect 可用于绘制删除面、影响边界和新增面。
dt_release_edit_result(effect);

dt_cdt_vertex_usage usage{};
dt_cdt_get_constraint_vertex_usage(cdt, boundary_id, 1, &usage);
uint32_t remove_flags = usage.constraint_count > 1
    ? DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH : 0;
dt_cdt_remove_constraint_vertex(cdt, boundary_id, 1,
                                remove_flags, nullptr);
// 默认保护共享顶点；显式标志只脱离当前约束，其他约束与基础点不变。

dt_cdt_constraint_edit edits[2]{};
edits[0] = {sizeof(dt_cdt_constraint_edit), DT_CDT_EDIT_UPDATE,
            boundary_id, 0, DT_CONSTRAINT_CLOSED,
            moved_boundary, boundary_point_count, {0, 0}};
edits[1] = {sizeof(dt_cdt_constraint_edit), DT_CDT_EDIT_ADD,
            0, DT_CONSTRAINT_BREAKLINE, 0,
            ridge, ridge_count, {0, 0}};
dt_constraint_id result_ids[2]{};
dt_cdt_apply_constraint_edits(cdt, edits, 2, result_ids, nullptr);
// 两项按顺序原子提交；任一失败则全部回滚，成功时只重建一次。

dt_cdt_query_result result = nullptr;
dt_cdt_query_triangles(cdt, &view_bounds, &result);
// 使用 dt_cdt_query_result_get_view() 读取域内三角形。
dt_cdt_release_query_result(result);

dt_grid_handle clipped_grid = nullptr;
dt_grid_from_cdt(cdt, &grid_options, &clipped_grid);
// 外边界以外和孔洞内部的节点为 NoData。
dt_grid_destroy(clipped_grid);
dt_cdt_destroy(cdt);
```

完整说明见 [docs/CDT_API.md](docs/CDT_API.md)。

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
# 可选第二参数比较逐条约束编辑和批量事务：
./build/dterrain_benchmark.exe 100000 12
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
- 一键切换透视 3D 地形，按高程分层设色并保留网格边；
- 3D 左键环视、右/中键平移、滚轮缩放、WASD/方向键漫游和 Q/E 升降；
- 0.5～8.0 倍垂直夸张、Home/“全图”相机适屏复位；
- “地形转换”菜单支持 TIN→GRID、GRID→TIN、TIN/GRID→等高线、
  等高线→TIN/GRID、TIN→CDT，以及 CDT→GRID/等高线；
- “图层”菜单控制 TIN、GRID 高程着色和等高线的显隐与叠加；
- 导入、导出 `DGRID 1` 与 `DCONTOUR 1` 文本数据；
- 打开、保存 `DCDT 1` 约束网，并叠加显示域内网格、外边界、孔洞和断裂线；
- “约束编辑”菜单支持逐点绘制断裂线、外边界、孔洞，Enter 完成、Backspace
  撤点、Esc 取消；还可移动或安全删除单个约束顶点并显示拓扑影响，或拾取删除整条约束；
- “批量添加 12 条示例断裂线”用一次原子事务演示批量约束导入并显示耗时；
- 全图复位及清空；
- 状态栏显示顶点数、三角形数、范围查询及编辑耗时。

详见 [docs/GUI.md](docs/GUI.md)。

安装或便携目录中的 `sample_data/sample_points.xyz`、`sample_grid.dgrid`、
`sample_contours.dcontour` 和 `sample_constraints.dcdt` 可用于快速验证四类文本
数据导入。
3D 演示仍使用轻量 GDI 软件渲染并按约 18,000 个三角形预算抽样；二维 GRID
预览缓存最多 400 万节点，等高线绘制预算约 20 万顶点。这些限制只影响演示显示，
不会删减 DLL 中的网格、GRID 或等高线数据。

## 已测性能

测试环境为本项目当前 Windows x64 Release/MinGW 构建，随机均匀 XY 数据：

| 点数 | 有限三角形 | 构建时间 | 峰值工作集 | 完整校验 |
|---:|---:|---:|---:|---:|
| 100,000 | 199,973 | 0.21 s | 未单独记录 | 0.02 s |
| 1,000,000 | 1,999,962 | 3.47 s | 460.5 MB | 0.36 s |
| 10,000,000 | 19,999,951 | 50.44 s | 4,553.2 MB | 3.39 s |

这些数据用于验证数量级，不代表所有坐标分布和硬件上的固定承诺。

同一 Release/MinGW 环境下，在 100,000 个基础点上添加 12 条互不相交断裂线：逐条
调用耗时 3.259 s，`dt_cdt_apply_constraint_edits()` 批量事务耗时 0.528 s，约
6.17 倍加速。该结果来自一次本机运行，用于说明避免 11 次重复重建的收益，不是固定承诺。

## 重要语义

- Delaunay 判定只使用 XY；查询参数中的 Z 不参与最近距离或点定位；
- 相同 XY 的两个点会返回 `DT_E_DUPLICATE_XY`；
- 修改高程使用 `dt_update_vertex_z()`，不会改变拓扑；
- 范围查询返回与闭合矩形相交的有限三角形；
- 旧接口返回的 `double*` 由 DLL 管理，只在同线程下一次旧接口调用前有效；
- 新接口结果必须通过对应的 `dt_release_*` 函数释放。
- GRID→TIN 遇到 NoData 默认拒绝；显式允许桥接会跨空洞构网，正式孔洞语义将在
  CDT 后端完成；
- 等高线→TIN/GRID 是由有限折点恢复连续表面的近似过程；等高线之外的信息无法
  唯一恢复，普通 TIN 也不强制保留原折线边；
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
