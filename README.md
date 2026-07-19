# dterrain 动态地形三角网 DLL

`dterrain` 是一个面向测绘地形建模的 C++17 动态库。它在 XY 平面上构建
Delaunay 三角网，把 Z 保存为顶点高程属性，支持批量建网、动态插入和删除、
编辑影响区、最近顶点、点定位、范围查询及保存加载。

当前稳定版本为 `1.0.0`，提供以下入口：

- `include/dt_api.h`：推荐使用的稳定 C ABI，支持多个三角网上下文；
- `include/dt_cdt_api.h`：折线约束、外边界、孔洞及域内三角形接口；
- `include/dt_terrain_api.h`：GRID、等高线及地形转换接口；
- `include/dt_task_api.h`：耗时转换的异步任务、进度与取消接口；
- `include/dt_gdal_api.h`：可选 GDAL 栅格/矢量数据交换接口；
- `include/dt_legacy.hpp`：题目给出的12个 C++ 接口的功能兼容层；
- `include/dterrain.h`：安装 SDK 的统一 C/C++ 入口；
- `include/dterrain.hpp`：可选 C++17 RAII 句柄与异常检查便利层。

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
- 转换与 GRID 概览异步任务、源数据生命周期保持、进度、等待、协作取消和任务自有结果。
- TIN、GRID、等高线 CRS WKT 元数据及转换传播；
- 可选 GDAL/OGR 通用栅格与线矢量导入、GeoTIFF/COG/GPKG 导出；支持 EPSG/WKT
  规范化、语义等价判断、批量点转换，以及 TIN、GRID、等高线显式重投影。
- 独立约束 Delaunay 句柄，支持断裂线、外边界、孔洞、约束增删、域内查询及
  `DCDT 1` 文本往返。
- 普通 TIN→CDT 初始化、CDT 域内高程采样，以及尊重外边界和孔洞的
  CDT→GRID/等高线转换。
- 保持稳定约束 ID 的原子几何更新，以及可选的新旧域网格影响结果。
- 约束顶点引用查询、共享顶点默认保护，以及显式的单约束脱离删除。
- 原子批量约束事务，一次提交多条新增、更新和删除并只重建一次候选 CDT。
- 可配置约束交叉策略：默认原子拒绝，或按空间索引自动节点化并校验两线插值 Z；
  无交叉断裂线使用局部 CGAL 拓扑插入，并提供编辑路径统计。
- `DCDTB 1` 约束网二进制交换、全载荷校验、固定范围目录、窗口索引查询和按 ID
  直接读取折线，无需构建整张 CDT 即可浏览约束元数据。
- TIN/CDT 三角面与“外环 + 多孔洞”的精确解析裁剪，返回片段、环和插值 XYZ，
  以范围索引限制候选面并报告面积守恒结果。
- 双 GRID 2.5D 平移配准、确定性 Halton 采样、可选 Z 偏差拟合、配准后重采样，
  以及带 RMSE 标准误差停止条件的自适应误差评估。
- GUI 任意地形剖面分析：两次单击定义 A—B，固定数据源采样并显示曲线、
  高程统计、累计升降和最大坡度，可导出 CSV。
- GUI 多边形面积与土方量测：逐点圈定区域，设置水平基准高程，报告平面面积、
  有效覆盖、地表面积、加权高程和挖填方估算，可导出 CSV。
- 普通 TIN、约束 CDT 与仿射 GRID 的统一坡度、下坡坡向、单位法向和支撑单元分析；
  GUI 单击显示支撑面/网格、下坡箭头和数值面板。
- 全幅 GRID 坡度、坡向与分析晕渲派生，保持 CRS、仿射变换和 NoData 语义；GUI
  提供固定量纲专题色带、图例、高程/专题切换及 DGRID/GeoTIFF 导出；异步任务
  支持进度与协作取消，分块多线程计算保持逐节点确定性结果，GUI 计算期间保持响应。
- 现状 GRID—设计 GRID 双表面土方分析：解析裁剪单元内零高差线，精确累计挖方、
  填方、净方、覆盖率及高差统计；支持 NoData、旋转仿射、分块并行、异步取消和
  可选高差 GRID。GUI 可加载设计面或从现状面生成偏移设计面，并显示蓝—白—红
  挖填专题及导出 CSV。
- 显式 GRID 重采样与设计面对齐：以参考 GRID 的行列数和完整仿射几何为目标，支持
  最近邻、双线性、严格/归一化 NoData、分块并行、异步进度和取消；CRS 不一致时
  明确拒绝，不静默重投影。
- 任意多边形 GRID 裁剪与掩膜：在世界 XY 中输入边界，支持保持原范围、紧凑裁剪和
  反向掩膜；完整仿射、源 NoData、CRS、并行、异步取消和原子结果发布保持一致。
- 内存受控的 GRID 窗口概览/LOD 读取：调用方直接提供小型输出缓冲区，可选平均、
  最近邻、最小值或最大值，支持 NoData、子窗口、精确整数分箱、并行和统计结果；
  同时提供任务自有缓冲区的异步入口；GUI 可预览超过 2000 万节点的高程及专题 GRID，
  不再复制整幅 double 数组或在绘制消息中等待聚合。
- 世界坐标视口到 GRID 源窗口的仿射裁剪：兼容北向上、旋转、剪切和负像元高，支持
  节点缓冲和范围外标志；GUI 在缩放、平移与拉框放大后自动读取局部窗口 LOD。
- `DGRIDB 1` 大 GRID 二进制格式：Windows 写时复制映射原始 double 节点，内置全幅
  概览、2× 多级金字塔与 4 MiB 原始数据块校验；支持视口 LOD、预取、按需校验缓存和
  主动完整性验证；保存时逐层逐行流式生成金字塔，不再同时驻留全部层级。
- 统一异步视口流水线：一个任务完成世界坐标裁剪、映射页预取、可选 DGRIDB 块校验和
  金字塔/精确 LOD，并同时返回源窗口、像素、统计与校验摘要；GUI 已直接使用该入口。
- 二维空间瓦片缓存：固定 LOD 瓦片目录、视口中心优先生产、跨请求在途合并、按字节和
  瓦片数双限额 LRU 淘汰；GUI 平移缩放复用相邻视口瓦片并显示复用计数。
- `DGTILE 1` 持久显示瓦片包：稀疏追加、启动时只扫描目录、命中时按需校验并读取单瓦片；
  GUI 为 DGRIDB 自动维护旁车文件，跨会话复用已浏览视口且失败时无损回退内存路径；
  支持在线校验压缩、空间回收和 Windows 跨进程单写保护。
- 跨层级渐进视口：同一异步任务按粗到细发布不可变 LOD 帧，支持序列读取、等待、取消和
  最终结果兼容；GUI 连续显示粗略帧、过渡帧和目标帧，不再等待全部细节完成后才换图。
- Windows GUI Direct3D 11 分块地形渲染：每 16,384 面一块，使用 GPU 深度缓冲和视锥
  裁剪，不再固定抽样 18,000 面；支持 Ctrl+左键 GPU ID 拾取、选中面高亮、WASD
  贴地漫游和硬件不可用时的软件回退。

## v1.0 SDK、发布门禁与目标规模验收

v1.0 冻结稳定 C ABI 的主版本号，并提供可安装的 `dterrain::dterrain` CMake 目标、
`dterrainConfig.cmake`、统一头文件、RAII 包装和独立下游消费工程。CPack 可生成包含 DLL、
导入库、运行时、头文件、示例、样例数据及手册的 ZIP SDK；现有 12 个兼容接口和
0.x 阶段文件格式保持可用。

新增导出门禁会从实际 DLL 验证 31 个关键入口。`tools/run_release_acceptance.ps1` 可完成
核心/GDAL 双构建、两轮 CTest、安装后消费工程、百万点/4096² GRID 压力套件及 ZIP 打包。
本机 v1.0 验收中，100 万随机点建网约 3.77 秒，128 轮插删约 11.6 毫秒，1000 次范围
查询约 10.3 毫秒；详细数据和适用边界见 `docs/RELEASE_CHECKLIST.md`。

## v0.85 精确边界、曲面配准与自适应误差

`dt_tin_clip_polygon_exact()` 与 `dt_cdt_clip_polygon_exact()` 先以多环多边形包围盒查询
TIN/CDT 空间索引，再逐候选三角面执行解析多边形交集。结果句柄保存片段—环—XYZ 点
层级，孔洞不会被三角扇错误填充；每个新边界点的 Z 都来自源三角面平面插值，
`exact_plan_area` 是所有交集片段的精确投影面积和。原有 `dt_grid_clip_polygon()` 仍是
节点掩膜接口，语义不变。

`dt_grid_register_surface()` 在共同 CRS 下用确定性 Halton 样本搜索待配准 GRID 的
XY 平移，并可为每个候选拟合常量 Z 偏差；结果明确约定把 `(dx,dy,dz)` 施加到移动面。
`dt_grid_apply_registration()` 将该面双线性重采样到参考 GRID 的完整六参数仿射几何。
当前模型有意不估计旋转、尺度或非刚性形变，也不执行隐式 CRS 重投影。

`dt_grid_compare_surface_adaptive()` 逐批累积平均误差、MAE、RMSE、最大绝对误差和覆盖率，
利用平方误差四阶矩估计 RMSE 标准误差；达到调用方阈值后提前停止并设置
`DT_SURFACE_ERROR_CONVERGED`。GUI 可对量测多边形执行 TIN/CDT 精确裁剪统计，并可把
已加载设计 GRID 配准到当前 GRID 后运行自适应误差评估。

## v0.70 约束网局部编辑、交叉策略与流式索引

`dt_cdt_options` 和 `dt_cdt_set_crossing_policy()` 可选择兼容旧行为的
`DT_CDT_CROSSING_REJECT`，或 `DT_CDT_CROSSING_SPLIT_COMPATIBLE_Z`。自动模式用
R-tree 只检测包围盒相交的线段，在真正交点分别线性插值 Z；两条约束的高程差不超过
调用方容差时，将同一个 XYZ 节点写入相关折线，否则整次编辑回滚。共线重叠仍明确拒绝。

拒绝模式下新增普通断裂线会复制当前候选状态，并直接调用 CGAL 局部约束插入；基础点
Delaunay 不再重建。`dt_cdt_get_edit_metrics()` 可区分局部拓扑更新和完整候选重建。
边界、删除、更新、批量事务及自动节点化仍采用完整候选状态，以维持域标记和事务原子性。

`dt_cdt_save/load_binary()` 使用 `DCDTB 1` 保存基础点、CRS、稳定约束 ID 和节点化折线，
并以 FNV-1a 校验全部载荷。固定 64 字节目录包含每条约束的类型、范围、点数和绝对偏移；
`dt_cdt_query_binary_index()` 可只扫描目录做窗口过滤，`dt_cdt_read_binary_constraint()`
按 ID 读取单条折线。GUI 同时打开/保存 `.dcdt` 文本和 `.dcdtb` 二进制，交互绘制默认启用
1 mm（当前 Z 单位）交点高程容差。

## v0.55 CRS 重投影与标准格式互操作

`dt_crs_normalize_wkt()` 接受 `EPSG:xxxx`、OGC WKT、PROJ 字符串等 GDAL user input；
`dt_crs_is_same()` 做语义等价判断；`dt_crs_transform_points()` 使用传统 GIS XY 轴序
批量转换，并在任何点失败时保持输出数组不变。

`dt_tin_reproject_gdal()` 转换全部顶点后在目标坐标系重新构建 Delaunay；
`dt_grid_reproject_gdal()` 默认由 GDAL 推导北向上目标范围和分辨率，也可通过
`DT_GDAL_REPROJECT_EXPLICIT_GRID` 指定节点中心仿射、宽高和最近邻/双线性/三次/
Lanczos 重采样；`dt_contours_reproject_gdal()` 保留折线分组与闭合标记。三者均返回
独立句柄，源对象不变，且不会改变已有重采样、土方或转换接口“不隐式重投影”的语义。

GUI 的“数据交换”菜单可打开 GDAL 支持的常见单波段地形栅格（如 GeoTIFF、DEM、
IMG、ASCII Grid、VRT）及 OGR 线矢量（如 GPKG、Shapefile、GeoJSON、KML），并可把
当前 TIN、GRID 或等高线显式重投影到整数 EPSG。成功后其他坐标系图层自动隐藏，避免
误叠加；工程对齐场景应在 DLL API 中使用显式目标 GRID 几何。

## v0.40 Direct3D 分块三维显示

GUI 的三维画布现由独立 Direct3D 11 子窗口承载。TIN 先以数据中心和 XY 半范围归一化，
再按 16,384 个三角形建立不可变顶点缓冲；每个块同时保存包围球，相机变化时在 CPU
执行视锥粗裁剪，GPU 对可见块执行真实深度测试和高程/光照着色。状态栏报告可见块/总块
以及本帧面数/总面数。硬件设备创建失败时自动尝试 WARP；仍失败才回退原 GDI 软件路径。

按住 Ctrl 单击三维画布会执行整数 ID 渲染通道，将三角形编号写入 `R32_UINT` 离屏目标，
只回读点击处一个像素；命中的三角形在普通着色通道中显示为橙色，并在状态栏报告中心
世界坐标。WASD/方向键移动默认开启贴地：程序用 DLL 的 TIN 点定位和线性高程插值调整
观察目标高度；按 F 可切换贴地，Q/E 仍可手动升降。相机射线、三角形相交、块切分、
视锥裁剪和贴地平滑均有无窗口单元测试，Direct3D 设备、绘制和 ID 拾取另有真实 GPU/WARP
冒烟测试。

## v0.37 跨层级渐进视口

`dt_grid_read_view_progressive_async()` 复用内存或 DGTILE 缓存，为一个世界视口按
`initial_lod_multiplier` 到目标 LOD 发布最多 8 帧。默认策略是 `4× → 2× → 1×目标`；
`dt_task_wait_for_grid_view_frame()` 可阻塞等待新序列，
`dt_task_get_grid_view_frame()` 按序返回尚未消费的不可变帧。每帧像素由任务持有到
`dt_task_destroy()`，后台发布细帧不会使旧指针失效。任务成功后，原有
`dt_task_get_grid_view_result()` 仍返回最终精度结果，因此旧调用路径无需迁移。
为限制稳定旧帧带来的内存占用，单任务全部帧的像素总量上限为 512 MiB。

预取和 DGRIDB 窗口块校验只在首帧执行一次，结果语义传播到后续帧；每个 LOD 仍使用
原二维瓦片目录、中心优先调度、跨请求合并、LRU 和持久旁车。新视口取消旧任务时，已经
发布的完整帧可继续作为上一画面，未完成帧不会外泄。GUI 每个 25 ms 轮询周期至多消费
一帧，保证即使后台很快完成，也会依次呈现粗、中、细三层，而非在一次绘制中跳过粗层。

4096×2048、1024×768 视口到 512×512 的本机 Release 基准中，默认三帧首帧约
5.18 ms、最终完成约 13.86 ms，LOD 序列为 `s4/s2/s1`；同一轮基准的单层冷缓存约
7.24 ms。数据只表示当前机器量级，渐进模式优化的是首个可用画面的延迟和交互连续性，
总工作量会高于只生成最终一帧。

## v0.36 DGTILE 生命周期管理

`dt_grid_view_cache_compact()` 将当前可写旁车原子重写为“每个键最后一条有效记录”，回收
坏瓦片恢复产生的历史重复记录，并在复制时校验全部保留负载。发现损坏的当前瓦片时将其
从新包省略，后续视口请求从源 GRID 重建；正式地形数据不受影响。96 字节结果结构报告
压缩前后字节数、回收空间、输入/输出记录数、重复记录和损坏瓦片数；磁盘统计也直接报告
已存记录数与无需读负载即可确定的历史记录可回收字节。普通内存缓存返回
`DT_E_NOT_FOUND`，只读旁车返回 `DT_E_UNSUPPORTED`。

压缩使用包级读写锁协调后台瓦片读取和追加，临时文件完成并刷新后才原子替换原包。
Windows 可写句柄同时持有由规范化绝对路径派生的命名互斥量；同一进程或另一进程再次
以写模式打开同一路径会返回 `DT_E_IO`，只读模式仍可按应用策略使用独立句柄。GUI 的
“数据交换 → 压缩当前 DGTILE 显示缓存”会先取消当前预览任务，再显示回收量和审计结果。

## v0.35 DGTILE 跨会话显示瓦片包

`dt_grid_view_cache_create_persistent()` 在 v0.34 内存瓦片目录下增加可选 `DGTILE 1` 旁车包。
包只保存实际访问过的固定 LOD 瓦片，打开时扫描 4 KiB 文件头和 128 字节记录头，不读取
全部像素；命中后才加载并校验对应 double 负载。源指纹综合调用方 `source_revision`、GRID
尺寸、仿射、CRS、NoData 和确定性抽样，过期包可按策略原子重建；损坏目录可重建，延迟
发现的损坏负载可丢弃并重新生成。容量耗尽或写入失败只停写旁车，当前显示继续从内存或
源 GRID 生成，不改变正式数据。

`dt_grid_view_cache_get_disk_statistics()` 报告包容量、文件大小、索引瓦片、磁盘命中、写入
和跳过次数。v0.36 已增加 Windows 跨进程单写保护与显式在线压缩；达到上限后可压缩，
也可直接删除并让其按需重建。完整布局见 [DGTILE_FORMAT.md](docs/DGTILE_FORMAT.md)。

GUI 打开或导出 `.dgridb` 后自动使用同名 `.dgridb.dgtile`，状态栏在真实磁盘瓦片命中时
显示“（磁盘）”。旁车目录不可写或包不可用时自动退回 v0.34 内存缓存。8192×4096 本机
基准中，128×128 瓦片、1024×768 视口到 512×512：首次填充约 14.47 ms，重建缓存句柄后
磁盘复用约 8.78 ms，48/48 瓦片来自持久包；数据只说明当前机器与文件系统量级。

## v0.34 二维空间瓦片缓存与跨视口合并

`dt_grid_view_cache_create()` 为一个源 GRID 创建独立缓存句柄；缓存键包含 GRID 数据代次、
2 的幂次 LOD、二维瓦片坐标、概览方法和影响数值的 NoData/金字塔策略。调用
`dt_grid_read_view_cached_async()` 后，当前视口涉及的瓦片按距视口中心由近到远进入共享
优先队列；另一请求命中相同的排队或加载瓦片时直接加入等待，不再重复读取或聚合。

缓存以 `maximum_bytes` 和 `maximum_tiles` 同时限流，完成瓦片按 LRU 淘汰；活动任务借用
的瓦片即使被目录淘汰也保持有效。`dt_grid_view_cache_get_statistics()` 报告命中、未命中、
在途合并、占用和淘汰计数，`dt_grid_view_cache_clear()` 清理当前未借用完成项。缓存句柄
共享持有源 GRID，因此公开 GRID 句柄可在缓存任务完成前释放。缓存路径是显示近似：
`DT_GRID_OVERVIEW_USED_TILE_CACHE` 明确表明统计覆盖返回像素，不替代分析用精确概览。

GUI 为当前显示源建立 128×128、128 MiB 的目录。状态栏显示“瓦片LOD”、源/输出尺寸、
LOD 比例以及复用瓦片数；切换源图层时重建目录，连续平移、滚轮和拉框继续沿用原有
旧帧保留、任务取消和结果代次核对。8192×4096 基准中，1024×768 视口到 512×512：
冷缓存约 6.64 ms，同视口热缓存约 3.30 ms（48/48 瓦片复用，约 2.01×）；水平平移
128 个源节点后复用 42 个瓦片，约 5.00 ms。数值只说明本机量级。

## v0.33 统一异步视口流水线

`dt_grid_read_view_async()` 把原先由调用方串联的 `dt_grid_get_view_window()`、
`dt_grid_prefetch_window()`、`dt_grid_verify_window()` 和 `dt_grid_read_overview()` 合并为
一个可等待、可取消、有单调进度的任务。`dt_grid_view_request_options` 直接接收世界坐标
范围、输出像素尺寸、概览方法、行块/线程参数以及预取、块校验、金字塔和严格 NoData
策略；完成后 `dt_task_get_grid_view_result()` 返回任务自有只读像素、实际源窗口、LOD
统计与可选校验摘要。借用内存仍只在 `dt_task_destroy()` 前有效。

校验阶段占进度 0～30%，LOD 阶段占 30～100%；未请求校验时 LOD 使用完整 0～100%。
视口无交集、文件块损坏、非法输出尺寸或协作取消均不会发布部分结果。GUI 保留原缓存键、
过期请求取消和失败抑制逻辑，但后台任务现在统一处理磁盘访问顺序，避免 UI 层分别预取、
校验再生成 LOD。旧同步/异步概览、窗口查询与校验接口保持不变。

4096×4096、1024×768 源窗口到 512×512 的本机基准中，统一任务提交约 54 μs，后台
完成约 2.65 ms，与旧同步窗口 LOD 的抽样校验和误差为 0；数据用于说明调度开销量级。

## v0.32 DGRIDB 流式金字塔保存

`dt_grid_save_binary()` 保持原有 C ABI 和 `DGRIDB 1` 文件布局，但保存实现不再为每一级
金字塔分配完整 `std::vector<double>`。第一层直接读取源 GRID，后续层每次只从临时文件
中的上一层读取两行、生成一行并写到目标偏移；内存峰值由约三分之一原始节点量降为约
2.5 行，且仍保留全幅概览、2× 多级金字塔、4 MiB 校验表和同目录原子替换。

普通内存 GRID 保存后不会映射刚写出的金字塔，以免其他句柄覆盖同一目标时受到 Windows
活动映射限制；重新加载文件即可获得映射金字塔。若句柄本身来自当前 DGRIDB，并覆盖保存
回原路径，库会先实体化写时复制修改、释放旧映射，原子替换后再把原始节点和金字塔映射
到新文件，当前句柄继续有效并保持映射/概览/金字塔/块校验能力。

8192×4096、343.19 MiB 本机基准中，旧完整金字塔临时数组理论占用 85.00 MiB，v0.32
逐行缓冲约 0.078 MiB；保存约 0.576 s，映射打开约 12.2 ms。4096×2048 时由 21.00 MiB
降至约 0.039 MiB，保存约 0.158 s。这些数字说明内存复杂度和本机量级，不是跨硬件承诺。

## v0.31 视口块校验与缓存

`dt_grid_verify_window()` 只验证与指定二维行列窗口相交的 DGRIDB 原始节点校验块，
`dt_grid_verify_window_async()` 把同一操作接入可等待、可取消、有进度的统一任务框架；
完成后由 `dt_task_get_grid_verification_result()` 取得块数、冷校验数、缓存命中数和实际
检查字节数。每个已加载 GRID 句柄保存线程安全的块状态：通过的块在后续视口请求中直接
命中缓存，已经失败的块继续立即报告 `DT_E_CORRUPTED_DATA`。

概览选项新增 `DT_GRID_OVERVIEW_VERIFY_SOURCE_BLOCKS`。它先校验当前源窗口涉及的原始
节点块，再读取持久概览、金字塔或精确源节点；GUI 对带块校验能力的 DGRIDB 自动启用，
因此缩放、平移、拉框和适屏的异步 LOD 会逐步覆盖用户实际浏览的数据。校验失败时不发布
新结果，保留上一幅有效画面并抑制同一失败请求的重复提交。窗口写入会清除该句柄的校验
能力和缓存，重新保存为 DGRIDB 后统一重建。

校验块固定按原始连续字节段每 4 MiB 划分，因此窗口触及一行中的少量节点也会验证相交
块的全部字节；结果中的 `checked_byte_count` 可能大于窗口自身数据量。该机制只覆盖原始
double 节点载荷，不校验持久概览或金字塔载荷；归档、复制传输或全文件审计仍应调用
`dt_grid_verify_binary_file()`，必要时配合外部 SHA-256。

8192×4096、343.19 MiB 本机基准中，冷态当前窗口验证 12 个块（48 MiB）约 58.56 ms，
同一窗口缓存复查约 6 μs，完整原始节点扫描约 384.06 ms；冷窗口路径约快 6.56×。
这些数字用于说明访问范围和缓存收益，不是跨硬件性能承诺。

## v0.30 异步视口 LOD 与请求取消

`dt_grid_read_overview_async()` 把窗口概览接入统一任务框架。任务复制 64 字节概览选项、
持有源 GRID 的共享生命周期并在内部拥有紧密排列的结果数组；完成后通过
`dt_task_get_grid_overview_result()` 取得 `dt_grid_overview_view`。其中 `values` 是只读借用
指针，在 `dt_task_destroy()` 前有效，避免跨 DLL 分配器释放和第二次结果复制。任务结果类型
为 `DT_TASK_RESULT_GRID_OVERVIEW`，进度、有限等待、错误文本和协作取消继续复用既有接口。

精确分箱和金字塔路径均在输出行边界检查取消；精确聚合扫描超大分箱时还会在源行内部
周期检查。并行路径以单调完成行数报告 0～1 进度，取消后不发布部分结果。源公开句柄可在
任务启动后释放，但任务运行期间不得并发写同一 GRID。

GUI 的 `WM_PAINT` 不再同步生成新 LOD。它保留上一幅缓存，提交后台视口请求并以 25 ms
定时器消费完成结果；连续滚轮、拉框、窗口调整或平移结束会取消过期请求，限制待回收任务
数量，并只发布最新源窗口和专题类型的结果。状态栏显示“异步LOD xx%”，完成后恢复
“金字塔LOD/局部LOD 源窗口→预览尺寸”。关闭、替换 GRID 或切换专题时会先协作取消并
安全回收任务。

4096×4096→512×512 本机基准中，同步自动并行约 14.69 ms，异步提交约 112 μs，后台完成
约 15.58 ms；立即取消 1×1 全窗口聚合约 0.36 ms 返回 `DT_TASK_CANCELLED`。这些数据用于
说明 UI 提交和取消延迟，不是对不同硬件的固定承诺。

## v0.29 DGRIDB 多级金字塔、预取与块校验

v0.29 在不改变 `DGRIDB 1` 版本号及旧头字段的前提下，把扩展目录、2× 平均金字塔和
原始节点校验表放在旧版全幅概览与原始节点段之间。v0.28 加载器会按头中记录的偏移
直接跳到原始节点，因此仍能打开 v0.29 文件；v0.29 同样继续读取没有扩展段的旧文件。
金字塔从 1/2 分辨率逐级生成，直到最长边不超过 512，每级独立按 64 KiB 对齐映射。

`DT_GRID_OVERVIEW_USE_PYRAMID` 允许平均概览从最合适的持久层级读取；这是显式、面向
显示的近似路径，结果用 `DT_GRID_OVERVIEW_USED_PYRAMID` 标识，未设置该标志时仍执行
逐源节点精确整数分箱。GUI 在缩放、平移结束和拉框放大后自动使用金字塔，并在状态栏
显示“金字塔LOD”。`dt_grid_prefetch_window()` 为即将访问的映射页面发出最佳努力的
操作系统预取提示。

保存时为原始节点每 4 MiB 记录一个校验值。加载仍保持惰性，不扫描数百 MB 数据；需要
审计时调用 `dt_grid_verify_binary_file()`，或在 GUI“数据交换→验证 DGRIDB 数据块”中
主动执行完整扫描。节点写入会使当前句柄的旧金字塔和校验状态失效，下一次二进制保存
自动重建。

8192×4096 本机基准文件约 343.19 MiB：保存 0.594 s、映射打开 5.17 ms、持久全幅概览
0.107 ms；4096×2048 局部窗口生成 512×256 预览时，精确路径 11.06 ms、金字塔路径
2.10 ms（约 5.26×）；完整块校验约 0.352 s。金字塔增加约三分之一磁盘空间，换取
多缩放级别的稳定局部读取性能。

## v0.28 DGRIDB 映射 GRID 与持久概览

`dt_grid_save_binary()` 把 GRID 保存为小端序 `DGRIDB 1`：固定 64 KiB 校验头、UTF-8
CRS、最多 512×512 的全幅平均概览，以及按行优先排列的原始 double 节点。保存先完整
写入同目录临时文件，刷新成功后才原子替换目标，失败不会留下半文件或覆盖已有结果。
完整布局见 [DGRIDB_FORMAT.md](docs/DGRIDB_FORMAT.md)。

`dt_grid_load_binary()` 仍返回普通 `dt_grid_handle`。Windows 下原始节点以写时复制方式
映射，打开不分配第二份完整数组；查询、视口 LOD、GRID→TIN、等高线、专题分析与
`dt_grid_write_window()` 均复用原接口。窗口编辑只修改私有页面，不会回写源文件；只有
显式二进制保存才提交。`dt_grid_info.flags` 用
`DT_GRID_STORAGE_MEMORY_MAPPED` 和 `DT_GRID_HAS_PERSISTENT_OVERVIEW` 报告当前能力。

GUI 的 GRID 导入/导出对话框同时支持 `.dgridb` 与原 `.dgrid/.txt`。DGRIDB 初次全图
适屏直接复制内置概览，缩放后仍结合 v0.27 视口窗口从映射节点按需读取。8192×4096
本机基准文件为 258.06 MiB：保存约 0.243 s，映射打开 6.65 ms，内置 512×512 概览
0.177 ms，1024×768 局部窗口读取 3.50 ms。具体结果受磁盘缓存和硬件影响。

## v0.27 视口自适应 GRID LOD

`dt_grid_get_view_window()` 把轴对齐世界 XY 视口通过 GRID 完整六参数逆仿射转换到连续
行列空间，再将得到的四边形裁剪到源节点覆盖域。64 字节 `dt_grid_view_options` 保存视口
范围和可选节点缓冲，64 字节 `dt_grid_window` 返回最小源行列窗口；完全不相交时返回
`DT_E_NOT_FOUND`，部分相交时设置 `DT_GRID_VIEW_WINDOW_CLIPPED`。返回窗口可以直接复制到
`dt_grid_overview_options.source_*`，不需要调用方自行处理旋转或剪切几何。

GUI 现在在滚轮缩放、平移结束、框选放大和全图复位后重新查询当前视口，仅把相交源
节点聚合为不超过 512×512 的局部位图，并把位图映射到该源窗口的仿射角点。全局色带、
等高线、分析和导出仍使用完整 GRID；状态栏显示“局部LOD 源窗口→预览尺寸”。平移拖动
期间复用旧缓存，松开后一次刷新，避免每个鼠标事件扫描源数据。

8192×4096 GRID 上，全图到 512×512 概览约 0.0128 s；1024×768 视口到同尺寸概览约
0.00264 s，视口几何查询约 15 μs，局部读取约快 4.85×。具体收益取决于视口占比、CPU
和内存带宽。

## v0.26 GRID 窗口概览与大栅格 LOD 预览

`dt_grid_read_overview()` 从任意源行列窗口直接写入调用方指定尺寸的 double 缓冲区，
不创建第二个 GRID 句柄。默认 `DT_GRID_OVERVIEW_AVERAGE` 用精确整数分箱覆盖源窗口，
保证每个源节点恰好进入一个输出格；另有最近邻、最小值和最大值模式。聚合模式默认
忽略 NoData 并按剩余有效值计算，`DT_GRID_OVERVIEW_STRICT_NODATA` 可要求分箱内任一
无效节点都使输出为 NoData。

64 字节 `dt_grid_overview_options` 支持子窗口、自动/显式线程数和行块高度；64 字节
`dt_grid_overview_result` 报告有效/无效数量及最小、最大、平均值。聚合模式会遍历完整
源窗口并设置 `DT_GRID_OVERVIEW_EXACT_SOURCE_STATISTICS`；最近邻统计只覆盖实际输出
样本，适合需要快速响应的调用。输出由调用方管理，接口没有跨 DLL 内存所有权。

GUI 的高程 GRID 和派生专题现在统一生成最多 512×512 的概览缓存；普通高程、坡度、
阴影和高差使用平均聚合，坡向使用最近邻以避免 359°/1° 的环形角度平均错误。由 GRID
生成等高线也不再依赖 2000 万节点的整幅 GUI 缓存。8192×4096 输入到 512×512 概览的
本机基准为单线程 0.0427 s、自动并行 0.00963 s，约 4.43×，串并行输出及均值一致。

## v0.25 任意多边形 GRID 裁剪与掩膜

`dt_grid_clip_polygon()` 按 XY 多边形筛选 GRID 节点，点的 Z 被忽略，首尾无需重复。
边界节点按区域内处理；自交边界采用确定性的偶奇填充规则。默认输出保持源 GRID 的
宽高和六参数仿射，只把区域外节点写为 NoData。设置
`DT_GRID_CLIP_CROP_TO_BOUNDS` 后，输出缩小为可能包含多边形的源节点行列包络，并
平移仿射原点；设置 `DT_GRID_CLIP_INVERT` 则保留区域外节点。紧凑裁剪与反向掩膜
不能组合。

实现先用源 GRID 逆仿射把多边形转换到连续列/行空间，所以北向上、旋转、剪切和负
像元高使用相同逻辑。源 NoData 始终保持无效；输出继承 CRS。异步入口
`dt_grid_clip_polygon_async()` 会深拷贝调用方多边形，支持进度与协作取消，失败或取消
不发布半成品。

GUI 复用“面积/土方量测”已完成的简单多边形，提供保持范围掩膜、紧凑裁剪适屏和
反向掩膜三个命令；成功后替换当前 GRID，可继续导出 DGRID/GeoTIFF、生成专题或转换
等高线。4096×4096、8 顶点多边形实测单线程 0.378 s、自动并行 0.0755 s，约 5.01×，
串并行抽样校验和一致。

## v0.24 显式 GRID 重采样与设计面对齐

`dt_grid_resample_like()` 把源 GRID 重采样到参考 GRID 的节点布局。目标节点先由参考
GRID 的六参数仿射变换映射到世界 XY，再通过源 GRID 仿射逆变换得到连续行列坐标，
所以旋转、剪切、负像元高和不同分辨率都使用同一条数学路径。输出尺寸、仿射和 CRS
继承参考 GRID，高程来自源 GRID；范围外节点写 NoData。

`DT_GRID_RESAMPLE_NEAREST` 适合分类或离散值，默认的
`DT_GRID_RESAMPLE_BILINEAR` 适合连续高程。双线性默认要求四个支撑节点全部有效；
`DT_GRID_RESAMPLE_RENORMALIZE_NODATA` 可显式按剩余有效权重归一化。源与参考 CRS WKT
必须完全一致，接口只做网格对齐而不执行坐标重投影。

异步入口 `dt_grid_resample_like_async()` 返回普通 `DT_TASK_RESULT_GRID`，可读取进度、
协作取消并通过 `dt_task_get_grid_result()` 取得结果。GUI 加载同 CRS 但未对齐的设计面
后，会保留原数据并提示用户选择“双线性对齐”或“最近邻对齐”；取消时原设计面保持
不变，对齐成功后可直接运行双表面挖填方。4096×4096 双线性基准实测单线程
0.243 s、自动并行 0.0538 s，约 4.53×，抽样校验和一致。

## v0.23 双表面 GRID 挖填方

`dt_grid_compare_earthwork()` 对节点对齐的现状面与设计面逐单元积分。每个 GRID
单元使用固定对角线分成两片线性三角面；当三角形内高差由正变负时，算法解析求出
零高差交线并分别积分正、负区域，不使用采样预算。`existing-design > 0` 计为挖方，
小于零计为填方。结果同时报告有效平面面积、覆盖率、最小/最大/平均高差和 RMSE。

两个 GRID 必须具有相同尺寸、六参数仿射变换和 CRS。默认任一角点对为 NoData 就
跳过整个单元；`DT_GRID_EARTHWORK_ALLOW_PARTIAL_CELLS` 可改为逐三角形判定。
`DT_GRID_EARTHWORK_OUTPUT_DIFFERENCE_GRID` 请求标准 `dt_grid_handle` 高差结果，
由调用方通过 `dt_grid_destroy()` 释放。异步入口
`dt_grid_compare_earthwork_async()` 复用任务进度、取消和源句柄生命周期保持，完成后
通过 `dt_task_get_earthwork_result()` 同时取得统计与可选高差 GRID。

GUI“分析”菜单可加载 DGRID 或 GDAL 设计面，也可把当前高程 GRID 整体偏移生成演示
设计面。计算期间保持窗口响应，`Esc` 可取消；完成后蓝色表示填方、红色表示挖方，
摘要可导出 CSV。4096×4096 起伏高差基准实测串行 0.439 s、自动并行 0.038 s，
约 11.50×，串并行体积误差为 0；该数据随 CPU、内存和数据分布变化。

## v0.22 分块并行地形专题分析

`dt_grid_terrain_options` 在原 32 字节 ABI 预留区中正式定义 `worker_count` 和
`tile_row_count`，结构总大小仍为 80 字节，既有零初始化调用保持二进制兼容。
`worker_count=0` 自动选择硬件线程（最多 32），`1` 强制单线程，显式值最大 64；
`tile_row_count=0` 选择 64 行一块。各工作线程只写互不重叠的输出行，调用线程统一
执行进度和取消回调，因此回调不会被并发调用，取消/失败也不会发布半成品。

GUI 的“设置专题分析与性能参数”除 z-factor 和光照角外，还可配置线程数及块高。
新增 `dterrain_terrain_benchmark` 可复现串行/并行比较。当前 Release/MinGW 构建在
4096×4096 GRID 上实测单线程 0.233 s、自动并行 0.0678 s，约 3.44×；64 个抽样
节点校验和一致。该结果只说明本机内存带宽和 CPU 条件下的数量级，不是固定承诺。

## v0.21 可取消的大栅格专题计算

`dt_grid_derive_terrain_async()` 把坡度、坡向与阴影地形派生接入统一任务框架。
任务持有源 GRID 的共享生命周期，使用 `dt_task_get_info()` 读取 0～1 进度，使用
`dt_task_request_cancel()` 请求协作取消，成功后通过 `dt_task_get_grid_result()`
取得标准 GRID 句柄。同步 `dt_grid_derive_terrain()` 保持兼容。

GUI 改用异步入口，并在等待期间处理窗口消息和刷新百分比；按 `Esc`、选择取消菜单
或计算时关闭窗口都会先安全取消任务。新增“设置专题分析与性能参数”，可配置高程单位系数
z-factor，以及阴影地形的太阳方位角和高度角。输出与 v0.20 相同，仍可导出 DGRID
或 GeoTIFF，原高程 GRID 不被覆盖。

## v0.20 全幅地形栅格分析与专题表达

`dt_grid_derive_terrain()` 从任意有效 GRID 派生同尺寸坡度、坡向或分析阴影 GRID。
坡度单位为度；坡向为以 +Y 为北、顺时针的最大下降方位角；水平节点的坡向写为
NoData；阴影值为 0～255，默认光源方位角/高度角为 315°/45°。算法在内部节点使用
中心差分，在边界使用单边差分，并通过完整六参数仿射雅可比把列/行导数转换到世界
XY。源节点或所需四邻域中出现 NoData 时，结果节点保持 NoData。

派生结果仍是普通 `dt_grid_handle`，可直接窗口读取、保存 DGRID 或通过 GDAL 写出
GeoTIFF/COG；尺寸、仿射变换和 CRS 自动继承。GUI 的“分析”菜单可从已有 GRID
生成三类专题图；只有 TIN/CDT 时会先自动生成 401 级 GRID。专题图拥有固定色带和
图例，不覆盖原高程 GRID，可随时恢复高程显示或独立导出。

```cpp
dt_grid_terrain_options analysis{};
analysis.struct_size = sizeof(analysis);
analysis.kind = DT_GRID_TERRAIN_SLOPE_DEGREES;
analysis.z_factor = 1.0;
analysis.output_nodata_value = -9999.0;

dt_grid_handle slope = nullptr;
dt_grid_derive_terrain(elevation_grid, &analysis, &slope);
dt_grid_save_text(slope, "slope.dgrid");
dt_grid_destroy(slope);
```

## v0.16 坡度、坡向与地形法向分析

三个地形句柄分别通过 `dt_analyze_tin_surface_xy()`、
`dt_cdt_analyze_surface_xy()` 和 `dt_grid_analyze_surface_xy()` 返回同一个
`dt_surface_analysis` 结构。结果包含采样高程、`dz/dx`、`dz/dy`、坡度角、
下坡坡向、向上的单位法向和实际参与计算的 3 或 4 个支撑点。

坡度以水平面为 0°，按 `atan(sqrt((dz/dx)^2+(dz/dy)^2))` 计算；坡向表示最大
下降方向，以 `+Y` 为北、顺时针 0～360°。水平面没有唯一坡向，此时设置
`DT_SURFACE_ASPECT_UNDEFINED`。普通 TIN 和 CDT 使用查询点所在三角面的解析平面；
边/顶点查询选择一个有效邻面，并通过标志报告位置。CDT 外域或孔洞、TIN 凸包外、
GRID 范围外或含 NoData 的支撑单元返回 `DT_E_NOT_FOUND`。GRID 使用完整六参数
仿射反算和 2×2 节点双线性导数，不要求网格轴与世界坐标轴平行。

GUI 中选择“分析→坡度/坡向分析（单击）”后，在二维地形上单击。黄色点表示查询
位置，青色虚线表示支撑三角形或 GRID 单元，青色箭头指向下坡方向，右上角面板
显示数据源、Z、坡度、坡向、梯度和单位法向。数据源优先级与剖面、面积量测一致：
可见 CDT、TIN、GRID 优先，其后才回退到已存在但隐藏的图层。按 `Esc` 或菜单命令
可清除结果；依赖的源图层变化时结果自动失效。

## v0.15 GUI 多边形面积与土方量测

“分析→面积/土方量测（逐点）”在二维画布中逐点圈定简单多边形，按 `Enter` 完成，
`Backspace` 撤回草图末点，`Esc` 清除。程序固定使用开始量测时选定的 CDT、TIN 或
GRID 表面，不在同一多边形内混用不同数据源；选择优先级与剖面分析一致。完成后
可修改水平基准高程并重新计算，或导出包含摘要和边界顶点的 UTF-8 CSV。

多边形平面面积与周长由边界坐标直接计算；地表面积、高程统计、挖方、填方和净挖方
采用约 20,000 个微三角形的数值积分。CDT 外域/孔洞、TIN 凸包外和 GRID NoData
按积分单元中心判定为无效，因此会同时报告“有效平面面积”和覆盖率。约定现状地形
高于基准面的体积为挖方，低于基准面为填方，净挖方 = 挖方 − 填方。这些三维量是
演示与研究用途的数值估算，不应直接替代工程结算所需的设计面、误差控制和质量验收。

## v0.14 GUI 任意地形剖面

“分析→任意剖面（两次单击）”在二维画布中依次选取 A、B 两点，并沿直线生成
401 个等距样本。整条剖面只使用一个数据源，优先级为：可见 CDT 有效域、可见
TIN、可见 GRID，其后才回退到当前存在但隐藏的数据；这样不会在同一剖面中逐点
混用不同表面。CDT 孔洞、TIN 凸包外和 GRID NoData 会保留为曲线断点。

画布显示青色 A—B 定位线和底部高程曲线，状态栏报告平距、有效样本、高程范围、
净高差、累计上升/下降及最大相邻样本绝对坡度。“导出剖面 CSV”写出距离、XYZ、
有效标志和相邻坡度，便于在表格或测绘分析软件中继续处理。GRID 采样按点读取
邻近 1×1 或 2×2 小窗口，不复制整幅大栅格。

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
# 专题栅格：宽、高、线程数（0 自动）、块高
./build/dterrain_terrain_benchmark.exe 4096 4096 0 64
# 双表面土方：宽、高、线程数（0 自动）、块高
./build/dterrain_earthwork_benchmark.exe 4096 4096 0 64
# GRID 重采样：宽、高、线程数（0 自动）、块高
./build/dterrain_resample_benchmark.exe 4096 4096 0 64
# GRID 多边形裁剪：宽、高、线程数（0 自动）、块高
./build/dterrain_clip_benchmark.exe 4096 4096 0 64
# GRID 概览：源宽、源高、输出宽、输出高、线程数（0 自动）、块高
./build/dterrain_overview_benchmark.exe 8192 4096 512 512 0 16
./build/dterrain_view_lod_benchmark.exe 8192 4096 1024 768 512 512
# DGRIDB：源宽、源高；报告保存、打开、精确/金字塔概览、预取、窗口/缓存及完整校验
./build/dterrain_grid_binary_benchmark.exe 8192 4096
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
- 一键切换 Direct3D 11 透视地形，按块上传、视锥裁剪、深度缓冲和高程光照着色；
- 3D 左键环视、右/中键平移、滚轮缩放、WASD/方向键贴地漫游、F 切换贴地和 Q/E 升降；
- Ctrl+左键执行 GPU ID 拾取并高亮三角形，状态栏显示可见块、绘制面数与拾取坐标；
- 0.5～8.0 倍垂直夸张、Home/“全图”相机适屏复位；
- “地形转换”菜单支持 TIN→GRID、GRID→TIN、TIN/GRID→等高线、
  等高线→TIN/GRID、TIN→CDT，以及 CDT→GRID/等高线；
- “图层”菜单控制 TIN、GRID 高程着色和等高线的显隐与叠加；
- 导入、导出 `DGRID 1` 与 `DCONTOUR 1` 文本数据；
- 打开、保存 `DCDT 1` 约束网，并叠加显示域内网格、外边界、孔洞和断裂线；
- “约束编辑”菜单支持逐点绘制断裂线、外边界、孔洞，Enter 完成、Backspace
  撤点、Esc 取消；还可移动或安全删除单个约束顶点并显示拓扑影响，或拾取删除整条约束；
- “批量添加 12 条示例断裂线”用一次原子事务演示批量约束导入并显示耗时；
- “分析”菜单支持两次单击生成任意 A—B 地形剖面、底部曲线和 CSV 导出；
- “分析”菜单支持逐点圈定简单多边形，设置水平基准高程并估算面积、地表面积和
  挖填方量，可导出摘要与边界顶点 CSV；
- “分析”菜单支持加载/生成设计面，对同几何现状与设计 GRID 执行精确双表面挖填
  分析，显示高差专题、实时进度与覆盖率，并导出土方摘要 CSV；
- “分析”菜单支持生成全幅坡度、坡向和阴影地形专题图，显示固定量纲图例，恢复
  高程着色，并将当前专题结果导出为 DGRID 或 GeoTIFF；计算显示实时进度，可用
  Esc/菜单取消，并可设置 z-factor、阴影光照角、工作线程数与分块行数；
- 全图复位及清空；
- 状态栏显示顶点数、三角形数、范围查询及编辑耗时。

详见 [docs/GUI.md](docs/GUI.md)。

安装或便携目录中的 `sample_data/sample_points.xyz`、`sample_grid.dgrid`、
`sample_contours.dcontour` 和 `sample_constraints.dcdt` 可用于快速验证四类文本
数据导入。
3D 演示在 Windows 上优先使用 Direct3D 11 分块绘制完整 TIN，并只提交视锥内块；设备
不可用时回退到约 18,000 面的 GDI 软件路径。二维 GRID 通过
`dt_grid_read_overview()` 生成不超过 512×512 的内存受控 LOD 位图，不再设置 2000 万
节点整幅缓存上限；等高线绘制预算约 20 万顶点。这些限制只影响演示显示，不会删减
DLL 中的网格、GRID 或等高线数据。

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
- 多边形量测的平面面积和周长由输入边界精确计算；地表面积、有效覆盖和挖填方为
  固定预算微三角形积分估算，精度受区域尺度、地形起伏、NoData 边界和数据源质量影响。

详细接口见 [docs/API.md](docs/API.md)，内部设计与限制见
[docs/DESIGN.md](docs/DESIGN.md)。

## Word 手册

- [dterrain DLL 开发与使用手册](docs/manuals/dterrain_DLL开发使用手册.docx)
- [dterrain GUI 操作手册与入门教程](docs/manuals/dterrain_GUI操作入门教程.docx)

手册生成脚本位于 `tools/build_manuals.py`，便于接口或 GUI 更新后同步维护文档。

## 许可证说明

本项目当前后端使用 CGAL 2D Triangulations 包。该包为 GPL 或商业许可双重模式；
本工程定位为研究和演示用途。分发或改变用途前应重新检查 CGAL 及其依赖的许可。
