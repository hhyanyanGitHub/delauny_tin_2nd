# GDAL 格式交换与 CRS 重投影 API

dterrain 0.55 的 `dt_gdal_api.h` 提供可选 GDAL/PROJ 适配层。它不暴露 GDAL C++ 对象，
调用方仍只使用稳定 C ABI 和 dterrain 句柄。

## 构建与能力探测

配置时设置 `-DDT_WITH_GDAL=ON`，并安装 vcpkg feature `gdal`。关闭该选项时所有
GDAL 入口仍存在，但返回 `DT_E_UNSUPPORTED`。运行时可调用：

```cpp
dt_gdal_initialize();
int32_t available = 0;
dt_gdal_is_driver_available("COG", &available);
```

本项目验证的精简依赖为 GDAL 3.11、PROJ 9.6、GeoTIFF 与 SQLite。自动测试覆盖
`GTiff`、`COG`、`GPKG`、EPSG:4326/3857 点转换及三类对象重投影。

## CRS 解析、比较和批量点转换

```cpp
size_t bytes = 0;
dt_crs_normalize_wkt("EPSG:4490", nullptr, 0, &bytes);
std::vector<char> wkt(bytes);
dt_crs_normalize_wkt("EPSG:4490", wkt.data(), wkt.size(), nullptr);

int32_t same = 0;
dt_crs_is_same(wkt.data(), "EPSG:4490", &same);

dt_point3 input[] = {{116.0, 40.0, 100.0}};
dt_point3 output[1]{};
dt_crs_transform_points("EPSG:4326", "EPSG:3857",
                        input, 1, output);
```

CRS 定义接受 GDAL `SetFromUserInput` 支持的 EPSG、WKT、PROJ 等形式。所有坐标转换
固定使用传统 GIS `X=经度/东、Y=纬度/北` 轴序。批量点先写内部临时数组；任一点非有限
或 PROJ 转换失败时，调用方输出保持不变。Z 会传递给 PROJ，因此使用复合/垂直 CRS 前
应确认垂直基准和所需网格文件。

## TIN、GRID 与等高线重投影

```cpp
dt_handle projected_tin = nullptr;
dt_tin_reproject_gdal(tin, "EPSG:3857", &projected_tin);

dt_gdal_reproject_options options{};
options.struct_size = sizeof(options);
options.target_crs = "EPSG:3857";
options.resample_algorithm = DT_GDAL_RESAMPLE_BILINEAR;
dt_grid_handle projected_grid = nullptr;
dt_grid_reproject_gdal(grid, &options, &projected_grid);

dt_contour_handle projected_lines = nullptr;
dt_contours_reproject_gdal(contours, "EPSG:3857", &projected_lines);
```

三类函数均返回独立句柄，源对象不变。TIN 转换全部顶点并在目标 XY 平面重新构建
Delaunay，因此顶点 ID 和三角形拓扑可能改变。等高线保持线分组、闭合标志和点序。
GRID 默认通过 `GDALSuggestedWarpOutput` 推导北向上范围、分辨率与尺寸，支持最近邻、
双线性、三次、三次样条和 Lanczos；NoData 默认继承源值，没有源 NoData 时使用 NaN。

需要与现有工程网格严格对齐时，设置 `DT_GDAL_REPROJECT_EXPLICIT_GRID`，并填写
`width`、`height` 和节点中心 `geo_transform[6]`。接口会在传给 GDAL Warp 前自动换算
为像元角点仿射。源对象没有 CRS、目标 CRS 无效、仿射奇异或转换失败时返回明确错误。
已有 `dt_grid_resample_like()`、土方和普通转换接口仍不会隐式重投影。

## GRID 与栅格

```cpp
dt_gdal_raster_save_options save{};
save.struct_size = sizeof(save);
save.driver_name = "COG";
const char* options[] = {"COMPRESS=DEFLATE", nullptr};
save.creation_options = options;
dt_grid_save_gdal_raster(grid, "terrain.tif", &save);

dt_grid_handle loaded = nullptr;
dt_grid_load_gdal_raster("terrain.tif", nullptr, &loaded);
```

导入入口使用 GDAL 自动识别驱动，不限 GeoTIFF；可读取 DEM、IMG、ASCII Grid、VRT 等
可用单波段栅格。默认读取第 1 波段，可通过 `band_index` 选择其他波段。数据转换为 `double`。
GDAL geotransform 描述左上像元角点，本库 geotransform 描述 GRID 节点；适配层将
像元中心与节点对应，因此往返不会产生半格平移。NoData 与投影 WKT 会被保留。

普通 `GTiff` 逐行写入，不复制整幅 GRID。`COG` 按 GDAL 驱动要求先建立临时
内存数据集再调用 `CreateCopy`，大栅格应为额外内存和临时存储留出余量。

## 等高线与矢量

```cpp
dt_gdal_contour_save_options save{};
save.struct_size = sizeof(save);
save.driver_name = "GPKG";       // NULL 时也是 GPKG
save.layer_name = "contours";
save.elevation_field = "elevation";
dt_contours_save_gdal_vector(lines, "terrain.gpkg", &save);

dt_contour_handle loaded = nullptr;
dt_contours_load_gdal_vector("terrain.gpkg", nullptr, &loaded);
```

输出层几何为 `LineStringZ`，属性包含双精度 `elevation` 和整数 `closed`。导入由 OGR
自动识别 GPKG、Shapefile、GeoJSON、KML 等可用线矢量格式，并接受
`LineString` 与 `MultiLineString`；优先从 elevation 字段取等高值，字段不存在时
回退到几何首点 Z。缺少二者会返回 `DT_E_CORRUPTED_DATA`。层 CRS WKT 会保存到
等高线句柄。

## 路径、选项和资源

- 文件名、driver、layer 和字段名均为 UTF-8；
- creation options 是以 `nullptr` 结束的 `NAME=VALUE` 字符串数组，只在调用期间
  借用；
- 导入返回的 GRID/等高线分别使用 `dt_grid_destroy()`、
  `dt_contours_destroy()` 释放；
- 启用 GDAL 的便携分发需同时携带构建目录中复制出的 GDAL 及其依赖 DLL，以及
  `share/proj` 数据目录。库初始化时会在 DLL 同级的 `share/proj` 自动查找
  `proj.db`；也可由宿主设置 `PROJ_DATA`。缺少 PROJ 数据库时 EPSG 解析和重投影会返回错误。
