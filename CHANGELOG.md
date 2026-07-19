# Changelog

## 1.0.0 — 2026-07-19

- 冻结稳定 C ABI 1.0 入口并新增统一 `dterrain.h`；
- 新增 C++17 RAII 便利头 `dterrain.hpp`；
- 新增可安装的 `dterrain::dterrain` CMake 包及 ZIP SDK；
- 新增独立下游消费工程和 31 个关键 DLL 导出门禁；
- 新增百万点 TIN、动态编辑、范围查询和大 GRID/DGRIDB 压力套件；
- 新增 CMake Presets、一键发布验收脚本、SDK 指南和发布清单；
- 收录 0.85 精确 TIN/CDT 多环裁剪、GRID 平移配准与自适应误差评估；
- 收录 0.70 局部 CDT 编辑、交叉自动节点化和 DCDTB 索引交换；
- 收录 0.55 CRS 重投影、GDAL 标准格式交换；
- 收录 0.40 Direct3D 11 分块显示、拾取和贴地漫游；
- 收录渐进 LOD、DGRIDB、DGTILE、地形互转、分析、量测与异步任务体系。

早期版本的详细阶段说明保留在 `README.md` 和技术设计文档中。
