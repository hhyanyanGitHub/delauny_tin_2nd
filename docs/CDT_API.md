# 约束 Delaunay API

本文说明 dterrain 0.70 的 `dt_cdt_api.h`。约束网使用独立 `dt_cdt_handle`，不会
改变普通 `dt_handle`、旧 12 接口或 TIN/GRID/等高线 API 的行为。

## 数据模型

CDT 仍是 2.5D 模型：约束判定和三角剖分只使用 XY，Z 是顶点高程。句柄保存两类
数据：

- 基础地形散点，由 `dt_cdt_build()` 一次性设置；
- 约束折线，由 `dt_cdt_add_constraint()` 添加并分配稳定的
  `dt_constraint_id`。

约束类型包括普通断裂线、闭合外边界和闭合孔洞。外边界及孔洞不依赖输入方向，
通过奇偶嵌套规则标记域：从无限面开始，每跨过一次边界，域状态翻转。普通断裂线
只强制成为三角网边，不改变域内外。

当没有外边界时，全部有限三角形均属于有效域。有孔洞时必须至少存在一个外边界。

## 创建、构建与约束增删

```cpp
dt_cdt_handle cdt = nullptr;
dt_cdt_create(nullptr, &cdt);
dt_cdt_build(cdt, points, point_count);
// 或从普通 TIN 复制全部顶点和 CRS，同时清除旧约束：
dt_cdt_build_from_tin(cdt, tin);

dt_constraint_id id = 0;
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                      ridge, ridge_count, &id);
dt_cdt_update_constraint(cdt, id, 0, moved_ridge, moved_count, nullptr);
dt_cdt_remove_constraint_vertex(cdt, id, 1, 0, nullptr);
dt_cdt_remove_constraint(cdt, id);
dt_cdt_destroy(cdt);
```

外边界和孔洞自动设置 `DT_CONSTRAINT_CLOSED`。闭合折线既可省略重复终点，也可将
首点作为末点传入；库会规范化为“不重复首点”的内部形式。连续重复 XY、基础点
重复 XY 或同一 XY 的高程冲突会被拒绝。

默认交叉策略保持兼容：未分段的交叉返回 `DT_E_UNSUPPORTED`，原句柄保持不变。
创建时填写 `dt_cdt_options.crossing_policy`，或随后调用
`dt_cdt_set_crossing_policy()`，可启用
`DT_CDT_CROSSING_SPLIT_COMPATIBLE_Z`。库使用 R-tree 筛选候选线段，计算 XY 交点并
分别沿两条线性插值 Z；差值不超过 `crossing_z_tolerance` 时，将统一交点加入相关
约束点序列。超过容差返回 `DT_E_INVALID_ARGUMENT` 并原子回滚。共线重叠始终返回
`DT_E_UNSUPPORTED`。

`dt_cdt_update_constraint()` 原子替换指定约束的点序列和闭合标志，约束类型及
`dt_constraint_id` 保持不变。更新失败时约束点、generation 和全部拓扑均不改变。
边界仍被强制闭合；断裂线可通过 `DT_CONSTRAINT_CLOSED` 在开闭状态间切换。

调用方可通过 `output_effect` 请求普通 `dt_edit_result`。结果包含更新前被移除的域内
三角形、更新后新增的域内三角形、删除区域边界以及新旧边，并使用更新后的 generation。
结果必须用 `dt_release_edit_result()` 释放。传入 `NULL` 时不会执行影响差分，可减少
大网更新的额外时间和内存。

### 共享顶点引用与受控删除

`dt_cdt_get_constraint_vertex_usage()` 按约束 ID 和点序号查询选中 XY 的引用情况：

- `constraint_count`：引用该 XY 的不同约束数量；
- `reference_count`：所有约束点序列中的总出现次数；
- `is_base_point`：该 XY 是否也是基础地形散点。

`dt_cdt_remove_constraint_vertex()` 删除目标约束中的一个点出现。默认情况下，若该
XY 被多条约束共同引用，函数返回 `DT_E_UNSUPPORTED`，原约束、generation 和拓扑
保持不变。调用方确认只需要从当前约束脱离后，可传
`DT_CDT_REMOVE_VERTEX_ALLOW_SHARED_DETACH`；其他约束仍保留共享点。基础地形散点
永远不会被此接口删除。

删除后的点序列仍须满足开折线至少 2 点、闭合约束至少 3 点、无零长度边和无未分段
交叉等规则。候选状态校验失败时操作原子回滚。`output_effect` 与约束更新接口相同，
可选返回完整的新旧域差异。

v0.70 在拒绝交叉模式下为新增普通断裂线提供局部拓扑路径：复制当前候选状态后只向
现有 CGAL CDT 插入新点和新约束边，不从全部基础点重新 Delaunay。边界新增、几何更新、
顶点/整条删除、批量事务和自动节点化仍完整重建候选网，以保持域屏障和原子语义。
`dt_cdt_get_edit_metrics()` 报告最后路径、局部更新次数、完整重建次数和 generation。

### 原子批量约束事务

`dt_cdt_apply_constraint_edits()` 按数组顺序处理 `ADD`、`UPDATE`、`REMOVE`：

- `ADD` 要求 `constraint_id == 0` 并提供类型、标志和点序列；
- `UPDATE` 要求已有稳定 ID，`kind == 0`，保留原约束类型；
- `REMOVE` 要求已有稳定 ID，且不携带点、标志或类型；
- 每个元素的 `struct_size` 必须是 `sizeof(dt_cdt_constraint_edit)`；
- 可选 `output_constraint_ids[i]` 在成功后返回对应新增或既有 ID；
- 可选 `output_effect` 对整批操作只计算一次完整域差异。

库先在内存约束记录上按顺序应用全部操作，再完整构建一个候选 CDT。任一参数、ID、
最小点数、交叉或孔洞规则失败时，原约束、ID 分配器、generation 和拓扑全部不变，
输出 ID 数组也不写入。成功时 generation 只增加一次。与逐条调用相比，N 项批量
事务把 N 次候选重建降为 1 次；它是 v0.11 面向大批量导入的性能路径，尚不等同于
原位局部拓扑更新。

```cpp
dt_cdt_constraint_edit edits[2]{};
edits[0].struct_size = sizeof(edits[0]);
edits[0].operation = DT_CDT_EDIT_UPDATE;
edits[0].constraint_id = existing_id;
edits[0].points = moved_points;
edits[0].point_count = moved_count;
edits[1].struct_size = sizeof(edits[1]);
edits[1].operation = DT_CDT_EDIT_REMOVE;
edits[1].constraint_id = obsolete_id;

dt_constraint_id ids[2]{};
dt_cdt_apply_constraint_edits(cdt, edits, 2, ids, nullptr);
```

## 域内查询

`dt_cdt_query_triangles()` 返回与闭合 XY 矩形相交的域内三角形。结果由独立句柄
拥有，必须调用 `dt_cdt_release_query_result()`：

```cpp
dt_cdt_query_result result = nullptr;
dt_cdt_query_triangles(cdt, &bounds, &result);
dt_cdt_query_result_view view{};
dt_cdt_query_result_get_view(result, &view);
// view.triangles 在 result 释放前有效。
dt_cdt_release_query_result(result);
```

`dt_cdt_statistics` 同时报告全部有限面数和域内面数，可用于检查边界/孔洞裁剪效果。

## 高程采样与派生转换

`dt_cdt_sample_height_xy()` 对有效域内点执行三角面线性插值。查询点在外边界外或
孔洞内部时返回 `DT_E_NOT_FOUND`；位于有效域边界和孔洞边界上时可取得边高程。

`dt_cdt_analyze_surface_xy()` 在同一个有效域三角面上返回采样 Z、世界 XY 梯度、
坡度角、下坡坡向、向上单位法向和 3 个支撑顶点。成功结果会填写公共结构尺寸：

```cpp
dt_surface_analysis result{};
dt_status status = dt_cdt_analyze_surface_xy(cdt, &query, &result);
```

查询点在外边界外或孔洞内返回 `DT_E_NOT_FOUND`。查询落在约束边、普通网边或顶点
时只从有效域邻面中选择一个支撑面，并设置 `DT_SURFACE_QUERY_ON_EDGE` 或
`DT_SURFACE_QUERY_ON_VERTEX`；外边界和孔洞边界上的有效侧仍可分析。坡向以 +Y
为北顺时针计量，表示最大下降方向；水平面设置
`DT_SURFACE_ASPECT_UNDEFINED`。完整字段与公式见 [API.md](API.md)。

`dt_grid_from_cdt()` 与 `dt_contours_from_cdt()` 直接使用域内三角形：

- GRID 仍使用 `dt_tin_to_grid_options` 指定尺寸、仿射变换和 NoData；
- 外边界以外及孔洞内部的 GRID 节点写为 NoData；
- 等高线在外边界或孔洞边界结束，不跨越无效域；
- CDT 的 CRS WKT 会传播到输出 GRID 或等高线句柄。

```cpp
dt_grid_handle grid = nullptr;
dt_grid_from_cdt(cdt, &grid_options, &grid);

dt_contour_handle contours = nullptr;
dt_contours_from_cdt(cdt, &contour_options, &contours);

dt_contours_destroy(contours);
dt_grid_destroy(grid);
```

## 约束枚举

先用 `dt_cdt_get_constraint_info()` 按索引取得 ID、类型、标志和点数，再用
`dt_cdt_copy_constraint_points()` 复制 XYZ。复制接口支持先传空缓冲区查询所需点数。

## 文本、二进制索引与 CRS

`dt_cdt_save_text()`、`dt_cdt_load_text()` 使用 `DCDT 1` 文本，保存基础散点、
约束 ID/类型/闭合标志、折线点和 CRS WKT。加载先完整解析并构建候选网，任何错误
都不会破坏原对象。

CRS 仅作为 UTF-8 WKT 元数据保存，不参与重投影。使用
`dt_cdt_set_crs_wkt()`/`dt_cdt_get_crs_wkt()` 访问。

`dt_cdt_save_binary()`、`dt_cdt_load_binary()` 使用小端 `DCDTB 1`。文件包括 4 KiB
头、基础点数组、固定 64 字节约束目录和连续折线点数组；目录记录稳定 ID、类型、标志、
点数、绝对偏移及 XY 包围盒。全部载荷带 FNV-1a 校验，完整加载前会先验证；也可调用
`dt_cdt_verify_binary_file()` 主动检查。

窗口浏览无需加载基础点或构建三角网：先调用 `dt_cdt_query_binary_index()` 传空缓冲
取得记录数，再分配 `dt_cdt_binary_index_entry` 数组读取与窗口相交的目录项。选中 ID 后，
`dt_cdt_read_binary_constraint()` 同样使用两次调用模式直接读取一条 XYZ 折线。

```cpp
dt_cdt_save_binary(cdt, "terrain.dcdtb");
dt_bounds2 window{500000, 3000000, 501000, 3001000};
uint64_t count = 0;
dt_cdt_query_binary_index("terrain.dcdtb", &window, nullptr, 0, &count);
std::vector<dt_cdt_binary_index_entry> index(count);
dt_cdt_query_binary_index("terrain.dcdtb", &window,
                          index.data(), index.size(), &count);
```

## 资源释放

| 对象 | 释放函数 |
|---|---|
| `dt_cdt_handle` | `dt_cdt_destroy()` |
| `dt_cdt_query_result` | `dt_cdt_release_query_result()` |
| `dt_edit_result`（约束更新/顶点删除可选结果） | `dt_release_edit_result()` |

约束点通过调用方缓冲区复制，不存在跨 DLL 的释放责任。
