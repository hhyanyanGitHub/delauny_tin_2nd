# GDAL 格式交换 API

dterrain 0.4 的 `dt_gdal_api.h` 提供可选 GDAL 适配层。它不暴露 GDAL C++ 对象，
调用方仍只使用稳定 C ABI 和 dterrain 句柄。

## 构建与能力探测

配置时设置 `-DDT_WITH_GDAL=ON`，并安装 vcpkg feature `gdal`。关闭该选项时所有
GDAL 入口仍存在，但返回 `DT_E_UNSUPPORTED`。运行时可调用：

```cpp
dt_gdal_initialize();
int32_t available = 0;
dt_gdal_is_driver_available("COG", &available);
```

本项目验证的精简依赖为 GDAL 3.11、PROJ 9.6、GeoTIFF 与 SQLite。已自动化验证
`GTiff`、`COG` 和 `GPKG` 驱动。

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

导入默认读取第 1 波段，可通过 `band_index` 选择其他波段。数据转换为 `double`。
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

输出层几何为 `LineStringZ`，属性包含双精度 `elevation` 和整数 `closed`。导入接受
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
  `proj.db`；也可由宿主设置 `PROJ_DATA`。本阶段 API 不执行重投影。
