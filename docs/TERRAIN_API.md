# GRID、等高线与转换 API

本文说明 dterrain 0.27 的 `dt_terrain_api.h` 和 `dt_task_api.h`。原
`dt_api.h`、旧 12 接口和 `.dtin/.dtmesh` 语义保持兼容。

## GRID 坐标模型

GRID 保存 `width * height` 个双精度高程节点。节点 `(column,row)` 坐标为：

```text
X = gt[0] + column * gt[1] + row * gt[2]
Y = gt[3] + column * gt[4] + row * gt[5]
```

这里的变换描述“节点位置”，不是 GDAL 的像元左上角语义。GDAL 适配层把像元
中心映射为 GRID 节点，导入导出自动转换半像元偏移。变换必须有限且可逆。

TIN、GRID 和等高线句柄都可保存可选 CRS WKT。使用 `dt_set_crs_wkt()`、
`dt_grid_set_crs_wkt()`、`dt_contours_set_crs_wkt()` 设置；对应 getter 采用先查询
所需字节数、再写入调用方缓冲区的方式。TIN、GRID、等高线之间的转换会传播 CRS，
但不执行重投影。

`dt_grid_read_window()`、`dt_grid_write_window()` 支持局部窗口；`row_stride`
以 `double` 个数计，零表示紧密排列。GRID 句柄由 `dt_grid_destroy()` 释放。

## GRID 窗口概览与 LOD 读取

`dt_grid_read_overview()` 把源 GRID 的任意行列窗口直接聚合到调用方拥有的小型
double 缓冲区，不创建第二个 GRID 句柄，也不复制整幅源数组：

```cpp
dt_grid_overview_options options{};
options.struct_size = sizeof(options);
options.method = DT_GRID_OVERVIEW_AVERAGE; // 0 也表示平均
options.source_column = 0;
options.source_row = 0;
options.source_width = 0;  // 到源 GRID 右边界
options.source_height = 0; // 到源 GRID 下边界
options.worker_count = 0;  // 自动最多 32；显式最多 64
options.tile_row_count = 16;

std::vector<double> preview(512 * 512);
dt_grid_overview_result statistics{};
dt_status status = dt_grid_read_overview(
    grid, &options, 512, 512, preview.data(), 512, &statistics);
```

| 方法 | 分箱行为 | 常见用途 |
|---|---|---|
| `AVERAGE` | 有效源节点算术平均；默认方法 | 连续高程、坡度、阴影和高差预览 |
| `NEAREST` | 取每个输出位置对应的源窗口中心样本；允许上采样 | 分类值、坡向角、极速预览 |
| `MINIMUM` | 每个整数分箱的有效最小值 | 保留洼地或下包络 |
| `MAXIMUM` | 每个整数分箱的有效最大值 | 保留峰值或上包络 |

平均、最小和最大模式要求输出宽高不超过源窗口。其分箱边界使用整数比例计算，所有
分箱无缝、无重叠地覆盖窗口，因此每个源节点恰好贡献一次。默认忽略 NoData；分箱
没有有效值时输出源 NoData（源无 NoData 时为 NaN）。设置
`DT_GRID_OVERVIEW_STRICT_NODATA` 后，只要分箱含一个无效节点，整个输出格就是
NoData。该标志不改变最近邻。

`dt_grid_overview_options` 和 `dt_grid_overview_result` 均固定为 64 字节。聚合模式的
结果统计覆盖完整源窗口，并设置 `DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS`；最近邻
统计只覆盖实际输出样本，因此该标志为空。结果结构可传 `nullptr`。`row_stride` 以
double 计，0 表示输出宽度；输出维度上限为每轴 1,048,576 且总计不超过十亿个值。

工作线程按输出行块写互不重叠的调用方区域，自动线程数最多 32，显式最多 64；所有
统计按输出行顺序归并，所以串并行输出和均值确定一致。接口是同步调用：返回前调用方
不得释放输出缓冲区，也不得并发修改源 GRID。

## 世界视口到 GRID 行列窗口

`dt_grid_get_view_window()` 为视口自适应 LOD 提供稳定的仿射几何入口：

```cpp
dt_grid_view_options view{};
view.struct_size = sizeof(view);
view.world_bounds = {xmin, ymin, xmax, ymax};
view.padding_nodes = 2;

dt_grid_window window{};
dt_status status = dt_grid_get_view_window(grid, &view, &window);
if (status == DT_OK) {
    dt_grid_overview_options overview{};
    overview.struct_size = sizeof(overview);
    overview.source_column = window.column;
    overview.source_row = window.row;
    overview.source_width = window.width;
    overview.source_height = window.height;
    // 输出尺寸应不大于 window.width × window.height
}
```

实现把世界矩形四角通过完整六参数逆仿射变成源行列四边形，再用 Sutherland–Hodgman
裁剪到节点覆盖域 `[-0.5,width-0.5] × [-0.5,height-0.5]`。随后以 `floor/ceil` 取得覆盖
所有相交节点的最小整数窗口，并在裁剪后应用 `padding_nodes`。所以旋转、剪切、负像元
高和单行/单列 GRID 使用相同规则。

两种结构均为 64 字节，`flags` 当前必须为 0，节点缓冲最大 1,048,576。完全无交集返回
`DT_E_NOT_FOUND`；视口部分超出 GRID 时成功并在结果设置
`DT_GRID_VIEW_WINDOW_CLIPPED`。接口只做几何计算，不读取高程、分配结果数组或修改 GRID，
因此适合在交互缩放路径中频繁调用。

## GRID 显式重采样与节点对齐

`dt_grid_resample_like()` 使用 `reference_grid` 定义输出行列数、六参数仿射和 CRS，
从 `source_grid` 取得高程：

```cpp
dt_grid_resample_options options{};
options.struct_size = sizeof(options);
options.method = DT_GRID_RESAMPLE_BILINEAR; // 0 也表示双线性
options.worker_count = 0;   // 自动，最多 32
options.tile_row_count = 0; // 默认 64 行
options.output_nodata_value = -9999.0;

dt_grid_handle aligned = nullptr;
dt_status status = dt_grid_resample_like(
    source, reference, &options, &aligned);
if (status == DT_OK) dt_grid_destroy(aligned);
```

对每个目标节点，库先用参考 GRID 变换计算世界 XY，再用源 GRID 变换的逆矩阵求连续
列/行坐标。该过程支持缩放、平移、旋转、剪切和负像元高，不把 GRID 限制为北向上。
源与参考 CRS WKT 必须完全一致；不一致返回 `DT_E_INVALID_ARGUMENT`，不会隐式调用
坐标重投影。目标落在源节点包络外时写输出 NoData。

| 方法/标志 | 行为 | 典型用途 |
|---|---|---|
| `DT_GRID_RESAMPLE_NEAREST` | 四舍五入到最近源节点 | 分类、编码或不允许平滑的数据 |
| `DT_GRID_RESAMPLE_BILINEAR` | 2×2 支撑节点加权；默认方法 | 连续高程、设计面和专题值 |
| `DT_GRID_RESAMPLE_RENORMALIZE_NODATA` | 忽略无效支撑并按有效权重重归一 | 明确接受 NoData 边缘外推时 |

双线性默认四个支撑节点任一无效即输出 NoData。归一化标志只在有效权重和大于零时
产生值；所有有效权重均为零仍写 NoData。输出总是带 NoData，零值
`output_nodata_value` 选择 NaN。`worker_count=0` 自动、`1` 串行、显式最大 64；
`tile_row_count=0` 表示 64 行，最大 1048576。

异步入口 `dt_grid_resample_like_async()` 的结果类型为 `DT_TASK_RESULT_GRID`。任务持有
源和参考 GRID 生命周期；用 `dt_task_get_info()` 查询进度、
`dt_task_request_cancel()` 请求取消，成功后以 `dt_task_get_grid_result()` 取得调用方
拥有的新句柄。失败或取消不发布半成品。

## GRID 任意多边形裁剪与掩膜

`dt_grid_clip_polygon()` 在世界 XY 中接收不少于三个 `dt_point3`；Z 被忽略，多边形
自动闭合。边界节点属于区域内，自交边界按偶奇规则确定内外：

```cpp
dt_point3 polygon[] = {
    {500000.0, 3200000.0, 0.0},
    {500500.0, 3200000.0, 0.0},
    {500450.0, 3200400.0, 0.0},
    {500050.0, 3200450.0, 0.0}
};
dt_grid_clip_options options{};
options.struct_size = sizeof(options);
options.flags = DT_GRID_CLIP_CROP_TO_BOUNDS;
options.worker_count = 0;   // 自动最多 32；显式最多 64
options.tile_row_count = 0; // 默认 64 行

dt_grid_handle clipped = nullptr;
dt_status status = dt_grid_clip_polygon(
    source, polygon, 4, &options, &clipped);
if (status == DT_OK) dt_grid_destroy(clipped);
```

| flags | 输出几何 | 保留节点 |
|---|---|---|
| `0` | 与源 GRID 完全相同 | 多边形内部及边界 |
| `DT_GRID_CLIP_CROP_TO_BOUNDS` | 缩小到多边形可能覆盖的源节点行列包络 | 多边形内部及边界 |
| `DT_GRID_CLIP_INVERT` | 与源 GRID 完全相同 | 多边形外部，不含边界 |

紧凑裁剪会平移仿射原点，像元列/行向量不变；若包络中没有任何源节点，返回
`DT_E_NOT_FOUND`。紧凑裁剪与反向掩膜组合没有明确意义，因此返回
`DT_E_INVALID_ARGUMENT`。输出始终带 NoData：`output_nodata_value=0` 时优先沿用源
NoData，否则选择 NaN。源中的 NaN/NoData 即使落在保留区域内也不会变为有效值。

实现把多边形一次性逆变换到源 GRID 连续行列空间，逐节点只执行边界/偶奇测试，支持
旋转、剪切和负像元高。`dt_grid_clip_polygon_async()` 启动前深拷贝输入点，调用方可
立即释放原数组；任务结果仍以 `DT_TASK_RESULT_GRID` 和
`dt_task_get_grid_result()` 提取。同步/异步取消或失败均不发布半成品。

## TIN 与 GRID

`dt_grid_from_tin()` 在每个 GRID 节点定位覆盖三角形，并在该三角形平面上进行
重心坐标线性插值。凸包外写入 NoData；该转换保持当前 TIN 的分片线性表面，不会
把顶点重新当作普通散点插值。

`dt_tin_from_grid()` 把所有有效节点作为 XYZ 点重建普通 Delaunay 网。规则 GRID
节点可能共圆，因此不承诺使用哪条单元对角线。包含 NoData 时默认返回
`DT_E_UNSUPPORTED`，原因是普通 Delaunay 会跨越空洞。只有调用方接受该语义时，
才能设置 `DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING`。边界和孔洞将在 CDT 阶段解决。

## 现状面—设计面双 GRID 土方

`dt_grid_compare_earthwork()` 比较两个节点对齐的 GRID：

```cpp
dt_grid_earthwork_options options{};
options.struct_size = sizeof(options);
options.flags = DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID;
options.worker_count = 0;   // 自动，最多 32
options.tile_row_count = 0; // 默认 64 个单元行

dt_grid_earthwork_result result{};
dt_grid_handle difference = nullptr;
dt_status status = dt_grid_compare_earthwork(
    existing, design, &options, &result, &difference);
if (status == DT_OK) {
    // result.cut_volume / fill_volume / net_volume
    dt_grid_destroy(difference);
}
```

输入约束：

- `width/height` 必须相同且至少为 2×2；
- 六参数仿射变换按相对 `1e-12` 容差一致，CRS WKT 必须完全一致；需要节点对齐时
  先显式调用 `dt_grid_resample_like()`，CRS 不一致仍须在外部重投影；
- `existing_z_factor/design_z_factor` 的零值均表示 1.0，非零值必须有限且大于零；
- `worker_count=0` 自动、`1` 串行、显式最大 64；`tile_row_count=0` 表示 64 行；
- 未知标志、过大块高、几何或 CRS 不兼容返回 `DT_E_INVALID_ARGUMENT`。

每个单元沿左上—右下固定对角线分成两片线性三角面。高差定义为经过各自
z-factor 后的 `existing-design`：正高差体积为挖方，负高差绝对体积为填方，
`net_volume=cut_volume-fill_volume`。若三角形跨越零高差，库解析计算两条边上的
零点并分别积分正负子多边形，因此结果不依赖抽样密度。世界平面面积使用完整仿射
行列式，旋转或错切 GRID 无需特殊处理。

`dt_grid_earthwork_result` 字段：

| 字段 | 含义 |
|---|---|
| `cell_count` | 全部四节点单元数 |
| `valid_triangle_count/skipped_triangle_count` | 参与或因 NoData 跳过的半单元数 |
| `total_plan_area/valid_plan_area/coverage_ratio` | 全域面积、有效积分面积及覆盖率 |
| `cut_volume/fill_volume/net_volume` | 挖方、填方与挖减填净方 |
| `minimum/maximum/mean_difference` | 有效区域高差范围及面积加权均值 |
| `rmse_difference` | 面积加权高差均方根 |

默认只要单元四对节点之一无效就跳过两片三角形，避免沿对角线产生半单元碎片。
设置 `DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS` 后改为逐三角形检查，可保留三个角点
都有效的半单元。无有效面积时覆盖率和体积为 0，高差统计为 NaN。

`DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID` 请求节点级高差 GRID，其尺寸、仿射与
CRS 继承现状面，任一输入节点无效时输出 NoData。同步调用必须同时传入
`output_difference_grid`；成功句柄归调用方并由 `dt_grid_destroy()` 释放。未设置该
标志时不会分配第二幅完整 GRID。

异步入口及结果取得方式：

```cpp
dt_task_handle task = nullptr;
dt_grid_compare_earthwork_async(existing, design, &options, &task);
// wait / progress / cancel
dt_grid_earthwork_result result{};
dt_grid_handle difference = nullptr;
dt_task_get_earthwork_result(task, &result, &difference);
dt_task_destroy(task);
```

任务结果类型为 `DT_TASK_RESULT_EARTHWORK`。任务持有两个源 GRID；取消或失败时不
发布统计或半成品高差 GRID。结果 getter 的高差输出参数可为 null；非空句柄仍归
调用方所有。

## 等高线

`dt_contours_from_tin()` 直接计算 TIN 三角面与水平面的交线；
`dt_contours_from_grid()` 使用一致的单元对角线把每个有效 GRID 单元分成两个
三角形。两者随后按 XY 容差拼接线段。

等高层有两种指定方式：

- `level_count > 0`：使用调用方给出的固定高程列表；
- `level_count == 0`：使用 `base + k * interval` 自动生成。

`dt_contours_get_line()` 返回的点数组由等高线句柄所有，只在句柄释放前有效。
闭合线设置 `DT_CONTOUR_LINE_CLOSED`。

当前版本对完全水平的平台三角形不输出平台边界；此类平台、断裂线和严格拓扑规则
将在 CDT/等高线增强阶段处理。

## 等高线反向生成 TIN/GRID

`dt_tin_from_contours()` 把等高线折点作为高程样本，原子替换调用方提供的普通
TIN。`dt_grid_from_contours()` 使用同一采样规则构造临时 TIN，再按
`dt_tin_to_grid_options` 插值 GRID。两者都以每条线的 `elevation` 为权威 Z，
不会信任点数组中可能不一致的 Z。

`dt_contours_to_tin_options` 控制采样：

- `maximum_segment_length == 0`：只使用原折点；正值会沿每段等距加点，保证采样
  段长不超过该值；
- `merge_tolerance == 0`：只合并完全相同 XY；正值会合并容差内且高程一致的点；
- 相同或容差内 XY 出现冲突高程时返回 `DT_E_CORRUPTED_DATA`；样本不足或全部共线
  时返回 `DT_E_EMPTY`。

反向转换并非数学逆运算：等高线之间和高低极值处的信息已经丢失，只能由采样点
恢复分片线性近似表面。输出是普通 Delaunay，原等高线段不一定成为三角网边；需要
硬边、外边界或孔洞时，应把折线作为约束加入 `dt_cdt_handle`。

## 异步任务

```cpp
dt_task_handle task = nullptr;
dt_grid_from_tin_async(tin, &options, &task);

int32_t completed = 0;
dt_task_wait(task, UINT32_MAX, &completed);

dt_task_info info{};
dt_task_get_info(task, &info);
if (info.state == DT_TASK_SUCCEEDED) {
    dt_grid_handle grid = nullptr;
    dt_task_get_grid_result(task, &grid);
    // 使用 grid
    dt_grid_destroy(grid);
}
dt_task_destroy(task);
```

任务在内部持有源 TIN/GRID 的共享生命周期，启动成功后可释放源公开句柄。取消为
协作式：转换循环会检查请求并返回 `DT_E_CANCELLED`。`dt_task_destroy()` 会请求
取消并等待工作线程退出。失败信息通过 `dt_task_get_error()` 读取。

v0.21 将全幅 GRID 专题派生接入同一框架：

```cpp
dt_task_handle task = nullptr;
dt_grid_derive_terrain_async(source, &terrain_options, &task);

for (;;) {
    int32_t completed = 0;
    dt_task_wait(task, 50, &completed);
    dt_task_info info{};
    dt_task_get_info(task, &info); // info.progress 为 0～1
    if (completed) break;
    // 用户取消时：dt_task_request_cancel(task);
}

dt_grid_handle derived = nullptr;
if (dt_task_get_grid_result(task, &derived) == DT_OK) {
    // 使用并最终 dt_grid_destroy(derived)
}
dt_task_destroy(task);
```

并行取消由协调线程每约 10 ms 检查一次，工作线程在行边界及超宽行内部周期检查。
取消任务不产生可取得的结果
句柄，`dt_task_info.state/result_status` 分别为 `DT_TASK_CANCELLED` 和
`DT_E_CANCELLED`。任务运行时源公开句柄可以释放，但不应并发写同一 GRID。

调用方在任务运行期间不应同时修改同一个 GRID；TIN 本身具有读写锁，但并发编辑
会使转换对应的版本不明确，仍建议先完成或取消转换再编辑。

## GRID 地表坡度、坡向与法向

`dt_grid_analyze_surface_xy()` 反算完整六参数仿射变换，在查询点所在 2×2 节点
单元内对高程进行双线性插值，并将列/行方向导数转换为世界 XY 的 `dz/dx`、
`dz/dy`。它返回公共头文件中的 `dt_surface_analysis`：

```cpp
dt_surface_analysis result{};
dt_point3 query{x, y, 0.0};
dt_status status = dt_grid_analyze_surface_xy(grid, &query, &result);
```

成功结果设置 `DT_SURFACE_BILINEAR`，`support_point_count` 为 4，支撑点顺序为
左上、右上、左下、右下的局部单元节点。边界节点由相邻的最后一个有效单元分析；
范围外、仿射变换不可逆或四个支撑节点中任一为 NoData/非有限值时返回错误，其中
无可用表面为 `DT_E_NOT_FOUND`。坡向定义、水平面标志和单位法向与普通 TIN 接口
完全一致，详见 [API.md](API.md)。

双线性单元一般不是一个平面，因此梯度、坡度和坡向是查询位置处的局部值；移动
查询点时这些值可以连续变化。旋转或错切 GRID 不应把列/行导数直接当作世界 X/Y
导数，本接口已完成该坐标变换。

## 全幅坡度、坡向与阴影 GRID

`dt_grid_derive_terrain()` 生成一个新的标准 GRID 句柄，源 GRID 不变。输出尺寸、
六参数仿射变换和 CRS 与源一致，并始终设置 `DT_GRID_HAS_NODATA`：

```cpp
dt_grid_terrain_options options{};
options.struct_size = sizeof(options);
options.kind = DT_GRID_TERRAIN_HILLSHADE;
options.z_factor = 1.0;
options.sun_azimuth_degrees = 315.0;
options.sun_altitude_degrees = 45.0;
options.output_nodata_value = -9999.0;
options.worker_count = 0;   // 自动，最多 32 个硬件线程
options.tile_row_count = 0; // 默认 64 行一块

dt_grid_handle derived = nullptr;
dt_status status = dt_grid_derive_terrain(source, &options, &derived);
// derived 可直接传给 dt_grid_save_text()/dt_grid_save_gdal_raster()
dt_grid_destroy(derived);
```

`kind` 支持：

| 值 | 输出量纲与范围 |
|---|---|
| `DT_GRID_TERRAIN_SLOPE_DEGREES` | 相对水平面的坡度角，0～90° |
| `DT_GRID_TERRAIN_ASPECT_DEGREES` | 最大下降方向，以 +Y 为北顺时针，0～360°；水平面为 NoData |
| `DT_GRID_TERRAIN_HILLSHADE` | 分析阴影灰度，0～255 |

`z_factor` 为高程到平面单位的换算系数，零选择 1.0，其余值必须有限且大于零。
阴影光源方位角按 +Y 为北顺时针，高度角范围为 -90～90°；两角都为零时使用
315°/45° 默认值。`output_nodata_value` 为零时自动选择 NaN，避免与有效的 0°坡度
或 0 灰度冲突；否则可以是有限数或 NaN。

v0.22 的并行选项使用原 ABI 预留区，`sizeof(dt_grid_terrain_options)` 仍为 80：

| 字段 | 零值 | 有效非零值 |
|---|---|---|
| `worker_count` | 自动读取硬件并发度，内部最多使用 32 | `1` 强制单线程；`2～64` 请求相应线程数 |
| `tile_row_count` | 64 行 | `1～1048576` 行；每次由一个工作线程领取一块 |

实际线程数还会受块数限制。并行只改变行调度，不改变任一节点的运算顺序，因此串行
与并行输出逐节点确定；各线程直接写最终 GRID 的不重叠行，不增加第二份全幅临时
数组。进度与取消回调始终由调用 `grid_derive_terrain()` 的协调线程串行调用，不会
从工作线程并发进入调用方代码。

每个内部节点分别用左右、上下节点中心差分，边界节点采用单边差分。计算先得到
`dz/dcolumn`、`dz/drow`，再用仿射雅可比逆转为世界坐标 `dz/dx`、`dz/dy`，因此
支持旋转和错切 GRID。中心及所需四邻域任一为 NoData/非有限数时，输出该节点为
NoData；这会在源空洞外形成一节点安全带，避免跨空洞估算坡面。

同步调用失败时 `*output_grid` 保持空。异步调用启动成功只表示工作线程已建立，
最终状态应通过任务 API 检查。宽或高小于 2 返回 `DT_E_EMPTY`，未知 kind、
非法系数、光照、线程数或块高返回 `DT_E_INVALID_ARGUMENT`。同步调用的时间和额外内存均为
O(width×height)，其中输出 GRID 占约 8×width×height 字节。

## 当前复杂度与大数据注意事项

- GRID 存储为连续 `double` 数组，约占 `8 * width * height` 字节；
- GRID 概览只分配与输出行数成比例的统计暂存；GUI 固定使用最多 512×512 输出，
  因此不再复制整幅源 GRID。聚合方法仍须读取选定窗口的全部节点，耗时与源窗口节点数
  线性相关；最近邻只读取输出样本；
- TIN→GRID 当前逐节点定位，适合正确性基线和中等 GRID；下一阶段会增加按三角形
  分块光栅化；
- 等高线生成不复制完整 GRID 三角面集，TIN 等高线也通过两次流式遍历三角形；
- 等高线线段及最终折线仍需驻留内存，后续将增加瓦片输出游标；
- 等高线反向转换的内存与加密后的唯一 XY 样本数近似线性；调用方应根据点数设置
  `maximum_segment_length`，避免过密采样；
- 单个 GRID 当前限制为十亿节点，实际应用还应根据 16GB 内存主动设置更低上限。

## 资源释放对照

| 对象 | 释放函数 |
|---|---|
| `dt_handle` | `dt_destroy()` |
| `dt_grid_handle` | `dt_grid_destroy()` |
| `dt_contour_handle` | `dt_contours_destroy()` |
| `dt_task_handle` | `dt_task_destroy()` |
| `dt_edit_result` | `dt_release_edit_result()` |
| `dt_query_result` | `dt_release_query_result()` |
