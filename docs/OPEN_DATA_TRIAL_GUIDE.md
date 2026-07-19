# 开源地形数据下载与 GUI 试用指南

本指南使用两组无需账号即可下载的公开样本，分别覆盖散点自动构网和真实 DEM
栅格处理。下载目录 `dist/sample_data/open_data` 已被 `.gitignore` 排除，不会把第三方
数据或 5 MB 栅格意外提交进源码仓库；下载脚本与本指南可以正常版本化。

## 1. 获取样本

在工程根目录打开 PowerShell：

```powershell
.\tools\fetch_open_terrain_samples.ps1
```

若 PowerShell 阻止本地脚本，可只为当前终端临时放行后再运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\fetch_open_terrain_samples.ps1
```

需要重新下载时使用 `-Force`。脚本下载到临时 `.part` 文件，完成后再替换目标文件，
并打印 SHA-256。当前验证版本应得到：

| 文件 | 用途 | 体量/预期结果 |
|---|---|---|
| `qgis_alaska_elevp.xyz` | XYZ 散点直接导入 | 150 点；构网后 286 个有限三角形 |
| `qgis_alaska_elevp.csv` | 保留的上游原始文件 | 150 条记录；含表头，不直接交给当前 XYZ 解析器 |
| `copernicus_glo90_fuji_n35_e138.tif` | 富士山区域 GLO-90 DSM | 1200×1200；1,440,000 个有效节点 |

数据来源：

- [QGIS Sample Data](https://github.com/qgis/QGIS-Sample-Data) 的
  [`qgis_sample_data/csv/elevp.csv`](https://raw.githubusercontent.com/qgis/QGIS-Sample-Data/master/qgis_sample_data/csv/elevp.csv)，由 Alaska GTOPO30 高程模型
  随机生成。该样本采用 Alaska Albers Equal Area，EPSG:2964，XY 单位为英尺。
  QGIS 样本仓库包含多来源数据，使用时应同时遵守仓库中列出的各来源许可。
- [Copernicus DEM GLO-90 公共 S3](https://registry.opendata.aws/copernicus-dem/) 的
  [N35E138 GeoTIFF](https://copernicus-dem-90m.s3.eu-central-1.amazonaws.com/Copernicus_DSM_COG_30_N35_00_E138_00_DEM/Copernicus_DSM_COG_30_N35_00_E138_00_DEM.tif)，覆盖东经 138°—139°、
  北纬 35°—36°，包含富士山。它是地表模型 DSM，不是去除建筑与植被后的裸地 DTM。

## 2. 启动正确的 GUI

GeoTIFF 交换依赖 GDAL，请运行当前已构建的 GDAL 版本：

```powershell
.\build-v04-gdal\dterrain_demo.exe
```

若“数据交换”菜单中的 GeoTIFF 项为灰色，说明运行的是无 GDAL 构建，或 GDAL 运行时
DLL 没有与 `dterrain_demo.exe` 放在一起。XYZ、DTMESH、DGRID、DGRIDB 和 DCONTOUR
文本/二进制功能不依赖 GDAL。

## 3. 快速路线：XYZ → TIN → 编辑 → 保存 → 3D

1. 单击“导入XYZ”，选择
   `dist/sample_data/open_data/qgis_alaska_elevp.xyz`。导入会自动建立 Delaunay TIN；
   状态栏应显示 150 个顶点，有限三角形数应为 286。
2. 使用滚轮以指针为中心缩放；右键或中键拖动平移。选择“框选放大”，左键拖出矩形
   后松开即可拉框适屏；`Esc` 取消正在进行的框选，“全图”恢复完整范围。
3. 选择“查询模式”并单击，检查最近顶点和覆盖三角形；再用“插入模式”添加一点，
   或用“删除模式”删除最近点，观察红色旧冲突面、边界和新边的更新效果。
4. 单击“保存网格”，先保存为 `.dtmesh` 文本，再用“打开网格”重载，核对顶点数与
   三角形数。需要更紧凑、更快的工程中间文件时另存为 `.dtin`。
5. 单击“切换3D”。左键拖动环视，右键/中键拖动平移，滚轮拉近拉远；`W/S/A/D`
   漫游，`Q/E` 调整观察目标高度，`+/-` 调整垂直夸张，`Home` 恢复三维适屏。

这组数据的坐标单位是英尺，适合检验大数值投影坐标、构网、编辑与三维相机；不要把
它和经纬度的 Copernicus 样本叠加或联合计算。

## 4. 真实 DEM 路线：GeoTIFF → GRID → 等高线/TIN → 导出

1. 选择“数据交换 → 导入 GeoTIFF / COG”，打开
   `copernicus_glo90_fuji_n35_e138.tif`。状态栏应显示 1200×1200 GRID；导入只替换
   GRID，不会把它自动变成 TIN。
2. 用滚轮、右键平移和“框选放大”检查大 GRID 的异步视口 LOD。放大后状态栏会显示
   源窗口到最多 512×512 预览的关系；LOD 只影响显示，不降低 DLL 内的原始数据。
3. 选择“地形转换 → 从 GRID 生成等高线（自动间隔）”。通过“图层”菜单分别显隐
   GRID 和等高线，检查两者叠加。然后在“数据交换”中导出 `.dcontour`；GDAL 可用时
   也可导出 GeoPackage 等高线。
4. 在“数据交换”中把当前 GRID 导出为 `.dgridb`，再重新打开它。该格式支持内存映射、
   内置概览、金字塔 LOD 和块校验；复制或归档后可执行“验证 DGRIDB 数据块”。也可
   导出普通 GeoTIFF 或 Cloud Optimized GeoTIFF，检查 CRS、NoData 和仿射变换往返。
   浏览几个局部区域后关闭再打开同一 DGRIDB，状态栏出现“（磁盘）”即表示命中了自动
   生成的 `.dgridb.dgtile` 跨会话显示缓存；该旁车可随时删除，不影响正式数据。
5. 选择“地形转换 → GRID → TIN（允许跨 NoData）”，再切换 3D。完整瓦片会建立约
   144 万顶点和约 288 万三角形，适合验证百万级链路，但比散点快速路线更耗时和内存。
   16 GB 内存下应关闭其他大内存程序，并等待状态栏报告转换完成。
6. 若只想查看局部，可先用“分析 → 面积/土方量测（逐点）”在二维 GRID 上圈一个简单
   多边形，至少三点后按 `Enter`；再执行“按量测多边形裁剪当前 GRID（紧凑适屏）”。
   裁剪后再做 GRID→TIN、等高线或 3D，速度更快。裁剪是节点级筛选，不等于工程结算
   所需的精确栅格单元边界裁切。

## 5. 三种地形形式的完整往返试验

推荐按以下顺序做一遍，每一步保存一个新文件，避免覆盖唯一副本：

```text
XYZ → TIN → DTMESH/DTIN
           ↓
          GRID → DGRIDB / GeoTIFF / COG
           ↓
        DCONTOUR / GeoPackage 等高线
           ↓
        TIN 或 GRID（近似重建）
```

- `TIN → GRID`：GUI 自动把最长边设为 401 个节点，适合演示，不代表生产项目的最佳
  分辨率；SDK 集成时应显式给定行列数、仿射变换和 NoData。
- `GRID → TIN`：当前普通 TIN 允许跨 NoData；需要真实外边界、孔洞或断裂线时，应转入
  CDT 约束流程。
- `等高线 → TIN/GRID`：只能从折点与线高程近似恢复表面，无法恢复等高线之间已经丢失
  的山顶、洼地等极值。

## 6. 坐标系与结果解释

- GUI 当前保存并恢复 CRS，但不执行重投影。一次试验只使用一种坐标系。
- Copernicus 文件是 EPSG:4326 经纬度。可以用它检验显示、格式交换和转换链路；但经纬度
  不是等距平面坐标，直接计算坡度、面积、体积和水平距离不具备严格的米制工程含义。
- 做定量分析前，应先在 QGIS/GDAL 中把目标区域重投影到合适的米制投影坐标系，再导入
  本工具。高程还需确认垂直基准和单位一致。
- Copernicus 产品是 DSM。若任务需要裸地地形、施工土方或水文分析，应改用经过地面点
  分类和滤波的 DTM/3DEP 等数据，并检查空洞、水面和异常值。

## 7. 扩展到其他公开数据

- 全球区域可继续使用 Copernicus GLO-90/GLO-30，按 1° 瓦片下载 GeoTIFF/COG。
- 美国区域可用 [USGS 3DEP 1/3 弧秒 DEM](https://data.usgs.gov/datacatalog/data/USGS%3A3a81321b-c153-416f-98b7-cc8e5f0e17c3)；
  该产品为约 10 m 的裸地高程、GeoTIFF 格式且属于公共领域，建议先裁剪到研究区。
- 激光雷达 LAS/LAZ 暂不能由当前 GUI 直接打开。可先用 PDAL/QGIS 过滤地面点并导出
  `x y z`，再通过“导入XYZ”自动构网；百万级以上建议分块、去重和按研究尺度抽稀。

公开数据的许可、版本、水平/垂直基准和质量说明可能更新。正式研究应把下载 URL、日期、
SHA-256、CRS、垂直基准、预处理步骤和软件版本一并记录到项目元数据中。
