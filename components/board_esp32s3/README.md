# board_esp32s3

ESP32-S3 板级设备树（从 `mini_tree` 拆出，不进入中间件 shelf）。

| 路径 | 说明 |
|------|------|
| `dts/board.dts` | 默认 `BOARD_DTS` 入口 |
| `dts/esp32-s3-devkitc-1.dts` | 同板别名/备份入口 |
| `dtsi/` | SoC / 总线 / 产品驱动片段 |

通用 `dt-bindings/` 仍在 `mini_tree/board/dt-bindings/`（dtc `-I` 指向该目录的父级 `board/`）。

板级自动发现：mini_tree（`cmake/esp_idf.cmake`）按 `board_${IDF_TARGET}` 约定发现本组件，
取 `dts/board.dts` + `dtsi/`（本目录 `CMakeLists.txt` 只是让 IDF 发现它的空注册）。
芯片 dtc `-I/-D` 由 `IDF_TARGET` 自动推导，无需任何注入文件。

dtc `-I` 可含 `mini_tree/board`（dt-bindings），**不要**把本目录 `dtsi/` 放进 `-I`
（否则 dtc-lite 会把 `.dtsi` 当厂商头做 cpp 抽宏而不内联，树只剩根节点）。
