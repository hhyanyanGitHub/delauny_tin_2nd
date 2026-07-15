# dterrain API 说明

## 生命周期

```cpp
dt_handle mesh = nullptr;
dt_status status = dt_create(nullptr, &mesh);
// 使用 mesh
dt_destroy(mesh);
```

每个 `dt_handle` 对应一个独立三角网。函数不会把 C++ 异常传出 DLL。失败时可调用
`dt_get_last_error()` 获取当前线程的错误文本。

0.4 的 GRID、等高线、转换和任务接口分别位于 `dt_terrain_api.h`、
`dt_task_api.h`，详见 [TERRAIN_API.md](TERRAIN_API.md)。可选 GDAL 交换接口位于
`dt_gdal_api.h`，详见 [GDAL_API.md](GDAL_API.md)。

TIN、GRID 和等高线句柄可保存可选 CRS WKT 元数据。getter 的 `required_size`
包含结尾 NUL；可先以 `buffer=nullptr, buffer_size=0` 查询大小。CRS 在地形形式
转换时传播，但当前不会自动重投影坐标。

## 批量构建

```cpp
dt_point3 points[] = {{0, 0, 10}, {1, 0, 20}, {0, 1, 30}};
dt_vertex_id ids[3];
dt_build(mesh, points, 3, ids);
```

批量构建具有事务性：参数或建网失败时保留原网；成功后整体替换。输入中相同 XY
被视为错误。`output_ids` 可为 `NULL`。

## 动态编辑

`dt_insert_point()`、`dt_delete_nearest_xy()` 和 `dt_delete_vertex()` 可返回
`dt_edit_result`。调用 `dt_edit_result_get_view()` 后可以读取：

- `removed_triangles`：编辑前被移除的有限三角形；
- `added_triangles`：编辑后新生成的有限三角形；
- `boundary_edges`：局部影响区边界；
- `removed_edges`、`added_edges`：线框增量；
- `affected_vertex_id` 和编辑后的 `generation`。

视图中的数组只在结果句柄释放前有效：

```cpp
dt_edit_result result = nullptr;
dt_insert_point(mesh, &point, &id, &result);
dt_edit_result_view view{};
dt_edit_result_get_view(result, &view);
// 消费 view
dt_release_edit_result(result);
```

## 查询

- `dt_find_nearest_vertex_xy()`：平面欧氏距离最近的顶点；
- `dt_locate_point_xy()`：返回面内、边上、顶点上、凸包外等分类；
- `dt_query_triangles()`：返回与 XY 矩形相交的三角形；
- `dt_get_statistics()`：顶点数、面数、维度、版本代和范围；
- `dt_validate()`：调用 CGAL 完整拓扑与 Delaunay 校验。

当前范围结果会一次性保存在结果对象中。GUI 应按当前视口查询，避免对千万级全网
一次性生成约 1.4GB 的 XYZ 输出。

## 保存加载

`.dtin` v1 保存顶点 ID 和 XYZ。加载时重新构建一个有效的 Delaunay 网并进行
事务替换。共圆点集可能得到另一条同样合法的对角线，因此 v1 不承诺逐面拓扑完全
一致；后续格式版本将增加拓扑块和校验码。

文本接口：

- `dt_import_points_text()`：读取逐行 XYZ 散点，成功后自动构网并原子替换当前网；
- `dt_save_mesh_text()`：保存 `DTMESH 1` 顶点和三角形索引文本；
- `dt_load_mesh_text()`：读取文本网格、重建 Delaunay，并逐面校验文本三角形；
- 三个接口的文件名均为 UTF-8，加载接口的 `output_bounds` 可以为 `NULL`。

文本格式的完整定义见 [TEXT_FORMATS.md](TEXT_FORMATS.md)。

## 旧接口数组布局

`pEffect` 的顺序为：

1. `f * 3` 个 XYZ 点，表示受影响三角形；
2. `h * 2` 个 XYZ 点，表示边界边；
3. `e * 2` 个 XYZ 点，表示新增边。

因此实际 `double` 数量为 `3 * [3f + 2(h + e)]`。数组由 DLL 所有，调用方不得
释放。
