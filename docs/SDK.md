# dterrain 1.0 SDK 集成指南

## 1. 交付结构

安装或解压 SDK 后，主要目录为：

- `bin/`：`dterrain.dll`、GUI 演示程序及运行时 DLL；
- `lib/`：MinGW 导入库和 `cmake/dterrain` 包配置；
- `include/`：稳定 C ABI、兼容层、统一入口 `dterrain.h` 和 C++17 RAII 包装
  `dterrain.hpp`；
- `docs/`：API、设计、格式、GUI 及 Word 手册；
- `examples/`、`sample_data/`：最小集成源码和可直接打开的数据。

## 2. CMake 集成（推荐）

```cmake
find_package(dterrain 1 CONFIG REQUIRED)
add_executable(my_terrain_app main.cpp)
target_link_libraries(my_terrain_app PRIVATE dterrain::dterrain)
```

配置时把 SDK 根目录加入搜索路径：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="D:/sdk/dterrain"
cmake --build build
```

运行时应让 `bin/` 位于可执行程序目录或 `PATH` 中。仓库中的
`tests/sdk_consumer` 是只依赖已安装 SDK 的完整冒烟工程。

## 3. C 与 C++ 入口

C/C++ 都可包含 `dterrain.h` 得到所有稳定 C ABI。跨 DLL 边界只传递固定宽度整数、
POD 结构、UTF-8 路径和不透明句柄；调用方不要释放 DLL 返回的内部指针，应使用对应
`destroy`/`release` 函数。

C++17 可包含 `dterrain.hpp`，使用 `dterrain::mesh`、`grid`、`cdt`、`task` 等
`std::unique_ptr` 型别自动释放句柄，并用 `dterrain::check()` 把失败状态转为包含线程局部
错误文本的 `std::runtime_error`。C ABI 仍是兼容性边界，RAII 层只是头文件便利包装。

```cpp
#include <dterrain.hpp>

auto tin = dterrain::make_mesh();
dterrain::check(dt_build(tin.get(), points.data(), points.size(), nullptr));
```

## 4. 构建预设

先设置 `VCPKG_ROOT`，然后可使用：

```powershell
cmake --preset windows-mingw-release
cmake --build --preset release
ctest --preset release
```

GDAL 完整版使用 `windows-mingw-gdal-release`；压力程序使用
`windows-mingw-stress`。MSVC 使用者可选择自己的生成器和与之匹配的 vcpkg triplet，
不能把 MinGW 导入库直接链接到 MSVC 工程。

## 5. 发布验收

```powershell
$env:VCPKG_ROOT = "E:/dev/vcpkg"
./tools/run_release_acceptance.ps1
```

脚本执行核心版与 GDAL 版编译测试、关键 DLL 导出校验、安装后下游工程构建、100 万点与
4096² GRID 压力程序，并生成 ZIP SDK。可用参数降低开发机负载；正式发布记录应保留默认
规模结果。

## 6. 二进制兼容约束

- 每个可扩展结构都先清零，并设置 `struct_size = sizeof(type)`；
- 新字段只通过保留空间或新结构/新函数增加；已有字段含义不改变；
- 调用 DLL 的编译器须接受 `__cdecl` 和标准 C 布局；
- 同一结果句柄可借用的数组只活到对应释放函数；
- 一个句柄的并发规则以 API 手册为准，不应在编辑期间并发读取同一对象；
- GDAL 能力由构建选项决定；无 GDAL DLL 对相关调用返回 `DT_E_UNSUPPORTED`。
