# GRID、等高线与转换 API

本文说明 dterrain 0.3 新增的 `dt_terrain_api.h` 和 `dt_task_api.h`。原
`dt_api.h`、旧 12 接口和 `.dtin/.dtmesh` 语义保持兼容。

## GRID 坐标模型

GRID 保存 `width * height` 个双精度高程节点。节点 `(column,row)` 坐标为：

```text
X = gt[0] + column * gt[1] + row * gt[2]
Y = gt[3] + column * gt[4] + row * gt[5]
```

这里的变换描述“节点位置”，不是 GDAL 的像元左上角语义。接入 GDAL 时，栅格
像元中心与节点 GRID 之间将由适配层明确转换。变换必须有限且可逆。

`dt_grid_read_window()`、`dt_grid_write_window()` 支持局部窗口；`row_stride`
以 `double` 个数计，零表示紧密排列。GRID 句柄由 `dt_grid_destroy()` 释放。

## TIN 与 GRID

`dt_grid_from_tin()` 在每个 GRID 节点定位覆盖三角形，并在该三角形平面上进行
重心坐标线性插值。凸包外写入 NoData；该转换保持当前 TIN 的分片线性表面，不会
把顶点重新当作普通散点插值。

`dt_tin_from_grid()` 把所有有效节点作为 XYZ 点重建普通 Delaunay 网。规则 GRID
节点可能共圆，因此不承诺使用哪条单元对角线。包含 NoData 时默认返回
`DT_E_UNSUPPORTED`，原因是普通 Delaunay 会跨越空洞。只有调用方接受该语义时，
才能设置 `DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING`。边界和孔洞将在 CDT 阶段解决。

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

调用方在任务运行期间不应同时修改同一个 GRID；TIN 本身具有读写锁，但并发编辑
会使转换对应的版本不明确，仍建议先完成或取消转换再编辑。

## 当前复杂度与大数据注意事项

- GRID 存储为连续 `double` 数组，约占 `8 * width * height` 字节；
- TIN→GRID 当前逐节点定位，适合正确性基线和中等 GRID；下一阶段会增加按三角形
  分块光栅化；
- 等高线生成不复制完整 GRID 三角面集，TIN 等高线也通过两次流式遍历三角形；
- 等高线线段及最终折线仍需驻留内存，后续将增加瓦片输出游标；
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
