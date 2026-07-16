# 文本文件格式

所有文本文件使用点号作为小数点。文件名通过 API 按 UTF-8 传递。

## XYZ 散点

每个有效行恰好包含三个有限浮点数：`x y z`。空格、制表符、逗号和分号均可
作为分隔符；支持空行、整行 `#` 注释和第三个坐标后的行尾注释。

```text
# x, y, z
500000.0, 3200000.0, 103.25
500010.0 3200000.0 104.10
500010.0;3200010.0;106.80 # ridge
```

调用 `dt_import_points_text()` 后立即按 XY 构建 Delaunay 网。文件为空、字段数
错误、坐标非有限或存在相同 XY 时导入失败，原三角网保持不变。

## DTMESH 三角网文本

```text
DTMESH 1
VERTICES 4
1 0 0 10
2 1 0 20
3 1 1 30
4 0 1 40
TRIANGLES 2
1 2 3
1 3 4
```

- `VERTICES` 行数为顶点数，每行依次为 `id x y z`；ID 为非零 `uint64_t`；
- `TRIANGLES` 行数为有限三角形数，每行是三个顶点 ID；
- `dt_save_mesh_text()` 使用足以无损往返 `double` 的十进制精度；
- `dt_load_mesh_text()` 先从顶点重建 Delaunay，再逐面检查索引、重复面、面数和
  Delaunay 拓扑；不匹配时返回 `DT_E_CORRUPTED_DATA`，原三角网保持不变；
- 当前格式用于普通 Delaunay TIN；约束网使用独立 `DCDT 1` 格式。

## DGRID 规则高程节点文本

```text
DGRID 1
SIZE 3 2
FLAGS 1
GEOTRANSFORM 500000 10 0 3200000 0 10
NODATA -9999
VALUES
100 101 102
103 104 -9999
END
```

- `SIZE` 为列数和行数；
- `FLAGS` 的位 0 表示启用 NoData；
- `GEOTRANSFORM` 按节点索引映射 XY，定义见 `TERRAIN_API.md`；
- `VALUES` 按行优先保存高程，共 `width * height` 个；
- `dt_grid_load_text()` 先完整校验并构建新句柄，不修改其他 GRID。

## DCONTOUR 等高线文本

```text
DCONTOUR 1
LINES 1
LINE 105 0 3
500000 3200000 105
500010 3200005 105
500020 3200010 105
END
```

`LINE` 后依次是等高值、标志和顶点数。标志位 0 表示闭合线；每个顶点保存 XYZ，
其中 Z 应等于该线的等高值。

## DCDT 约束三角网文本

```text
DCDT 1
CRS "LOCAL_CS[\"sample\"]"
POINTS 4
0 0 10
10 0 12
10 10 15
0 10 13
CONSTRAINTS 1
CONSTRAINT 1 2 1 4
0 0 10
10 0 12
10 10 15
0 10 13
END
```

- `CRS` 使用带转义的双引号字符串，可为空；
- `POINTS` 保存基础地形 XYZ；
- `CONSTRAINT` 后依次为约束 ID、类型、标志和点数；类型 1/2/3 分别表示断裂线、
  外边界、孔洞，标志位 0 表示闭合；
- 闭合折线不重复保存首点；
- 加载会重建 CDT、标记奇偶嵌套域并验证约束；失败时原句柄不变。
