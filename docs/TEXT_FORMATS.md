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
- 当前格式用于普通 Delaunay TIN。后续约束 Delaunay 将通过新格式版本增加约束段。
