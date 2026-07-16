# 约束 Delaunay API

本文说明 dterrain 0.7 的 `dt_cdt_api.h`。约束网使用独立 `dt_cdt_handle`，不会
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

dt_constraint_id id = 0;
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_BREAKLINE, 0,
                      ridge, ridge_count, &id);
dt_cdt_remove_constraint(cdt, id);
dt_cdt_destroy(cdt);
```

外边界和孔洞自动设置 `DT_CONSTRAINT_CLOSED`。闭合折线既可省略重复终点，也可将
首点作为末点传入；库会规范化为“不重复首点”的内部形式。连续重复 XY、基础点
重复 XY 或同一 XY 的高程冲突会被拒绝。

当前交叉约束必须预先在交点处分段，并把交点作为共享顶点传入。未分段的交叉返回
`DT_E_UNSUPPORTED`，原句柄保持不变。

v0.7 为正确性基线：添加或删除约束会在候选状态中完整重建 CDT，成功后原子替换。
这适合研究、文件交换和中等规模约束编辑；百万级实时局部约束编辑将在后续版本
增加。

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

## 约束枚举

先用 `dt_cdt_get_constraint_info()` 按索引取得 ID、类型、标志和点数，再用
`dt_cdt_copy_constraint_points()` 复制 XYZ。复制接口支持先传空缓冲区查询所需点数。

## 文本与 CRS

`dt_cdt_save_text()`、`dt_cdt_load_text()` 使用 `DCDT 1` 文本，保存基础散点、
约束 ID/类型/闭合标志、折线点和 CRS WKT。加载先完整解析并构建候选网，任何错误
都不会破坏原对象。

CRS 仅作为 UTF-8 WKT 元数据保存，不参与重投影。使用
`dt_cdt_set_crs_wkt()`/`dt_cdt_get_crs_wkt()` 访问。

## 资源释放

| 对象 | 释放函数 |
|---|---|
| `dt_cdt_handle` | `dt_cdt_destroy()` |
| `dt_cdt_query_result` | `dt_cdt_release_query_result()` |

约束点通过调用方缓冲区复制，不存在跨 DLL 的释放责任。
