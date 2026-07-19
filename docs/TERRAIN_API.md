# GRID、等高线与转换 API

本文说明 dterrain 0.33 的 `dt_terrain_api.h` 和 `dt_task_api.h`。原
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

## DGRIDB 二进制映射 GRID

```cpp
dt_status saved = dt_grid_save_binary(grid, "terrain.dgridb");

dt_grid_handle mapped = nullptr;
dt_status opened = dt_grid_load_binary("terrain.dgridb", &mapped);
if (opened == DT_OK) {
    dt_grid_info info{};
    info.struct_size = sizeof(info);
    dt_grid_get_info(mapped, &info);
    const bool mapped_values =
        (info.flags & DT_GRID_STORAGE_MEMORY_MAPPED) != 0;
    dt_grid_destroy(mapped);
}
```

`DGRIDB 1` 保存完整六参数仿射、NoData、UTF-8 CRS、源统计、最多 512×512 的全幅平均
概览、2× 多级平均金字塔、4 MiB 原始节点块校验表和按行优先排列的 double 节点。
Windows 加载使用私有写时复制映射，原始节点和各金字塔层按需调页，句柄仍可传给全部
现有 GRID API；窗口写入不会修改源文件。保存采用同目录临时文件加成功后原子替换。
保存回当前映射源时，为解除 Windows 映射锁定会在替换前实体化当前视图；另存为其他
路径不需要完整副本。v0.32 保存时逐层逐行生成金字塔：第一层读取源 GRID，后续层仅从
临时文件上一层读取两行并直接写一行，峰值临时内存与 GRID 行宽线性相关，不再与总节点
数线性相关。文件布局和加载兼容性不变。格式字段见 [DGRIDB_FORMAT.md](DGRIDB_FORMAT.md)。

普通内存 GRID 保存后保留持久概览和校验能力，但不让当前句柄映射新文件的金字塔，以免
活动映射阻碍其他句柄原子覆盖目标；重新加载即可取得 `DT_GRID_HAS_PYRAMID`。覆盖当前
映射源时，保存句柄在替换后重新映射原始节点和各层金字塔，因此继续报告全部 DGRIDB
能力。若应用需要保存后立即使用金字塔 LOD，推荐销毁普通内存句柄并加载刚保存的文件。

`DT_GRID_STORAGE_MEMORY_MAPPED`、`DT_GRID_HAS_PERSISTENT_OVERVIEW`、
`DT_GRID_HAS_PYRAMID` 和 `DT_GRID_HAS_BLOCK_CHECKSUMS` 是
`dt_grid_info.flags` 的输出能力位，不允许作为 `dt_grid_create_options.flags` 输入。
对完整源范围、默认 NoData 策略、平均方法和文件内记录尺寸的概览请求会直接复制持久
概览；写入任意节点后旧概览自动失效，下一次二进制保存重建。DGRIDB 是本机高性能
格式；文本交换仍用 DGRID，GIS 互操作使用 GeoTIFF/COG。

加载不会为校验而扫描全部原始节点。需要传输、归档或故障排查级完整性检查时显式调用：

```cpp
dt_status checked = dt_grid_verify_binary_file("terrain.dgridb");
```

该函数重新加载头部并逐一验证 4 MiB 原始节点块；无扩展的 v0.28 文件返回
`DT_E_UNSUPPORTED`，校验不一致返回 `DT_E_CORRUPTED_DATA`。对即将进行精确窗口读取的
映射 GRID，可先调用 `dt_grid_prefetch_window()` 发出最佳努力的操作系统预取提示；普通
内存 GRID 上该函数成功但不执行额外操作。

### 视口原始节点块校验

v0.31 可只验证当前准备访问的源窗口，并把结果缓存到已加载 GRID 句柄：

```cpp
dt_grid_verify_result verification{};
verification.struct_size = sizeof(verification);
dt_status checked = dt_grid_verify_window(
    mapped, column, row, width, height, &verification);
// verification.block_count：相交校验块总数
// verified_block_count：本次实际读取并通过的块数
// cached_block_count：本次直接命中句柄缓存的块数
```

`dt_grid_verify_result` 固定为 64 字节。DGRIDB 原始节点段按连续字节每 4 MiB 分块；实现把
二维窗口的逐行字节区间映射为去重块集合，所以 `checked_byte_count` 统计完整相交块，可能
大于窗口节点本身的字节数。重复调用会设置 `DT_GRID_VERIFY_USED_CACHE`。缓存属于当前
加载句柄且受互斥保护；并发只读校验安全，但竞争线程可能重复计算同一冷块。任一已知失败
块会继续立即返回 `DT_E_CORRUPTED_DATA`。窗口写入会撤销持久概览、金字塔、块校验能力
及其缓存，调用方不得与校验并发修改同一 GRID。

异步入口复用统一任务的进度、等待、错误与取消协议：

```cpp
dt_task_handle task = nullptr;
dt_grid_verify_window_async(mapped, column, row, width, height, &task);
// ... dt_task_wait / dt_task_get_info / dt_task_request_cancel ...
dt_grid_verify_result verification{};
verification.struct_size = sizeof(verification);
dt_task_get_grid_verification_result(task, &verification);
dt_task_destroy(task);
```

该任务结果类型为 `DT_TASK_RESULT_GRID_VERIFICATION`。任务共享持有源 GRID，成功提交后
调用方可释放公开源句柄；取消在块边界协作生效，取消或失败时 getter 不返回部分结果。
无块校验扩展的旧 DGRIDB 和普通内存 GRID 返回 `DT_E_UNSUPPORTED`。

若读取概览时必须先验证其源窗口，可设置：

```cpp
options.flags |= DT_GRID_OVERVIEW_VERIFY_SOURCE_BLOCKS;
```

校验占任务进度的前 20%，随后仍可直接复制持久全幅概览或读取金字塔。这个标志验证的是
对应原始 double 节点块，不是概览/金字塔载荷本身；它适合交互式逐视口防护，不能替代
归档后的 `dt_grid_verify_binary_file()` 全量审计。8192×4096 本机基准中，12 个相交块
（48 MiB）冷校验约 58.56 ms，缓存复查约 6 μs，完整扫描约 384.06 ms。

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

平均方法还可显式启用显示型金字塔路径：

```cpp
options.flags |= DT_GRID_OVERVIEW_USE_PYRAMID;
dt_grid_overview_result result{};
result.struct_size = sizeof(result);
dt_grid_read_overview(grid, &options, 512, 256, pixels, 0, &result);
bool approximate =
    (result.flags & DT_GRID_OVERVIEW_USED_PYRAMID) != 0;
```

库选择仍不小于输出尺寸的最低分辨率持久层级。金字塔像元是逐级 2×2 忽略 NoData 的
平均，因此该路径是可预期的显示近似，结果统计覆盖实际读取的金字塔像元，不设置
`DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS`。严格 NoData、最小值、最大值、最近邻、没有
持久金字塔或层级分辨率不足时自动回到原始节点路径；不设置该标志时旧精确语义不变。

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
统计按输出行顺序归并，所以串并行输出和均值确定一致。同步接口返回前调用方不得释放
输出缓冲区，也不得并发修改源 GRID。

v0.30 增加任务自有结果缓冲区的异步入口：

```cpp
dt_task_handle task = nullptr;
dt_grid_read_overview_async(grid, &options, 512, 512, &task);

int32_t completed = 0;
dt_task_wait(task, UINT32_MAX, &completed);
dt_grid_overview_view view{};
view.struct_size = sizeof(view);
if (dt_task_get_grid_overview_result(task, &view) == DT_OK) {
    // view.values[row * view.row_stride + column]
    // view.result 保存统计和精确/金字塔标志
}
dt_task_destroy(task);
```

任务复制选项并持有源 GRID；调用方可在启动成功后释放源公开句柄。`view.values` 是只读
借用指针，紧密排列且只在任务销毁前有效，不得由调用方释放或写入。结果类型为
`DT_TASK_RESULT_GRID_OVERVIEW`。精确和金字塔路径均按输出行报告进度和检查取消，超大
精确分箱还在源行内部检查；取消状态不发布部分数组。GUI 连续视口请求可取消旧任务并
立即提交新任务，旧任务完成后只需销毁而不提取结果。

v0.33 为交互显示新增统一世界视口任务，避免调用方手工串联窗口映射、预取、校验和 LOD：

```cpp
dt_grid_view_request_options request{};
request.struct_size = sizeof(request);
request.flags = DT_GRID_VIEW_REQUEST_PREFETCH_SOURCE |
                DT_GRID_VIEW_REQUEST_VERIFY_SOURCE_BLOCKS |
                DT_GRID_VIEW_REQUEST_USE_PYRAMID;
request.world_bounds = {xmin, ymin, xmax, ymax};
request.output_width = 512;
request.output_height = 384;
request.padding_nodes = 2;
request.overview_method = DT_GRID_OVERVIEW_AVERAGE;

dt_task_handle task = nullptr;
dt_grid_read_view_async(grid, &request, &task);
int32_t completed = 0;
dt_task_wait(task, UINT32_MAX, &completed);

dt_grid_view_result result{};
result.struct_size = sizeof(result);
if (dt_task_get_grid_view_result(task, &result) == DT_OK) {
    // result.source_window：实际源节点窗口
    // result.values：result.width × result.height，只读借用像素
    // result.overview / result.verification：LOD 统计与块校验摘要
}
dt_task_destroy(task);
```

请求选项为固定 96 字节。平均、最小和最大概览的输出尺寸仍不得超过实际源窗口；最近邻
允许上采样。预取是最佳努力提示；请求块校验但句柄没有 `DT_GRID_HAS_BLOCK_CHECKSUMS`
时，任务以 `DT_E_UNSUPPORTED` 失败。校验阶段映射到进度 0～0.3，LOD 映射到 0.3～1；
未请求校验时 LOD 使用全部进度范围。任务持有源 GRID，失败或取消不发布结果；成功结果
类型为 `DT_TASK_RESULT_GRID_VIEW`，其中所有指针在 `dt_task_destroy()` 后失效。

v0.34 在相同结果生命周期上增加可复用二维空间瓦片缓存：

```cpp
dt_grid_view_cache_options cache_options{};
cache_options.struct_size = sizeof(cache_options);
cache_options.tile_width = 128;
cache_options.tile_height = 128;
cache_options.worker_count = 4;
cache_options.maximum_bytes = 128ULL * 1024ULL * 1024ULL;
cache_options.maximum_tiles = 4096;

dt_grid_view_cache_handle cache = nullptr;
dt_grid_view_cache_create(grid, &cache_options, &cache);

dt_task_handle task = nullptr;
dt_grid_read_view_cached_async(cache, &request, &task);
int32_t completed = 0;
dt_task_wait(task, UINT32_MAX, &completed);

dt_grid_view_result result{};
result.struct_size = sizeof(result);
if (dt_task_get_grid_view_result(task, &result) == DT_OK) {
    // result.lod_scale：缓存 LOD 的 2 次幂源节点间距
    // result.tile_count / reused_tile_count：总瓦片与复用瓦片数
    // result.overview.flags 含 DT_GRID_OVERVIEW_USED_TILE_CACHE
}
dt_task_destroy(task);

dt_grid_view_cache_statistics statistics{};
statistics.struct_size = sizeof(statistics);
dt_grid_view_cache_get_statistics(cache, &statistics);
dt_grid_view_cache_clear(cache);   // 清理当前未被任务借用的完成项
dt_grid_view_cache_destroy(cache);
```

默认瓦片为 128×128、容量 128 MiB/4096 项，自动生产线程最多 8；显式线程数最大 64，
瓦片边长允许 16～1024。缓存任务从视口中心向外调度，目录中的相同排队/加载瓦片由并发
请求共享。完成项同时按字节和数量执行 LRU 淘汰；活动任务借用期间可暂时超过容量，释放
后立即收敛。缓存共享持有源 GRID，销毁公开 GRID 或缓存句柄不会破坏已经提交的任务。

缓存路径使用固定 2 次幂空间 LOD 并对输出像素重采样，是交互显示近似，不应用于体积、
坡度、等高线等精确分析。`DT_GRID_VIEW_RESULT_USED_TILE_CACHE` 标识该路径，命中已完成项
或加入在途项分别设置 `CACHE_HIT`、`CACHE_COALESCED`。`overview` 统计只覆盖返回像素，
设置 `DT_GRID_OVERVIEW_USED_TILE_CACHE` 且不设置 `EXACT_SOURCE_STATISTICS`。GRID 写入后
generation 改变，新请求自动使用新目录键，不会读到旧代次瓦片。

### DGTILE 持久显示缓存（v0.35）

需要跨进程会话保留已浏览热点时，用 `dt_grid_view_cache_create_persistent()` 代替内存版
create；后续提交、等待、取结果和销毁流程完全相同：

```cpp
dt_grid_view_disk_cache_options disk{};
disk.struct_size = sizeof(disk);
disk.flags = DT_GRID_VIEW_DISK_CACHE_RESET_STALE |
             DT_GRID_VIEW_DISK_CACHE_RESET_CORRUPTED;
disk.utf8_file_name = "terrain.dgridb.dgtile"; // 创建时复制路径内容
disk.source_revision = application_revision;   // 数据改变时必须改变
disk.maximum_file_bytes = 2ULL * 1024 * 1024 * 1024;

dt_grid_view_cache_handle cache = nullptr;
dt_status status = dt_grid_view_cache_create_persistent(
    grid, &cache_options, &disk, &cache);
if (status == DT_OK) {
    // 与内存缓存相同：dt_grid_read_view_cached_async(cache, ...)
    dt_grid_view_disk_cache_statistics stats{};
    stats.struct_size = sizeof(stats);
    dt_grid_view_cache_get_disk_statistics(cache, &stats);
    dt_grid_view_cache_destroy(cache);
}
```

`dt_grid_view_disk_cache_options` 固定 64 字节。文件名是 UTF-8，库在 create 返回前复制为
内部路径；字符串随后可释放。`source_revision` 由应用定义，零也合法。库还把尺寸、仿射、
CRS、NoData 和最多 64×64 个确定性源样本加入指纹，但抽样不能替代 revision。默认容量
为 2 GiB，最大 1 TiB。`READ_ONLY` 要求文件已存在；`RESET_STALE` 在源指纹或瓦片尺寸
变化时原子重建；`RESET_CORRUPTED` 重建坏头/目录，并允许在按需读取发现坏负载时重新
生成该瓦片。只读模式不会执行两种 reset。

`dt_grid_view_disk_cache_statistics` 固定 96 字节，报告容量、当前文件字节数、索引键数、
目录命中次数、成功写入数、停写/容量跳过数和源指纹。`DISK_CACHE_HIT` 结果标志表示至少
一个负载实际来自磁盘；仅查到但校验失败并重算的瓦片不设置该标志。对普通内存缓存调用
磁盘统计返回 `DT_E_NOT_FOUND`。`dt_grid_view_cache_clear()` 只清理内存层，不删除旁车。

缓存只在源 GRID generation 与 create 时一致时查写旁车；写入后继续显示但绕过旧包。
包满、只读或写失败不会使视口任务失败。单进程同一路径只允许一个可写缓存，可并存多个
只读缓存；尚无跨进程写锁。文件为追加式且不在线压缩，关闭所有句柄后可安全删除并重建。
完整布局见 [DGTILE_FORMAT.md](DGTILE_FORMAT.md)。

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

`DT_TASK_RESULT_GRID_OVERVIEW` 使用 `dt_task_get_grid_overview_result()` 返回任务拥有的
借用视图；v0.31 的 `DT_TASK_RESULT_GRID_VERIFICATION` 使用
`dt_task_get_grid_verification_result()` 返回按值复制的 64 字节校验统计；`GRID`、
`CONTOURS` 和 `EARTHWORK` 结果仍分别按原 getter 提取。异步概览特别
适合高频视口调度，因为提交不借用调用方输出数组，任务被取消或丢弃时也无需处理半成品。

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
| `dt_grid_view_cache_handle` | `dt_grid_view_cache_destroy()` |
| `dt_contour_handle` | `dt_contours_destroy()` |
| `dt_task_handle` | `dt_task_destroy()` |
| `dt_edit_result` | `dt_release_edit_result()` |
| `dt_query_result` | `dt_release_query_result()` |
