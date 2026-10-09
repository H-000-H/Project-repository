# Rust 上层应用组件 —— no_std staticlib 路线（路线 ②）
#
# 分工：cargo +esp（乐鑫 fork）交叉编译到 xtensa-esp32s3-none-elf，产出 libapp_rust.a，
#       再由 ESP-IDF CMake 链接进固件。Rust 导出 app_main（ESP-IDF 入口），
#       直接 FFI 调 mini_tree / driver_ws2812 / cJSON / esp_log，app 逻辑不再有 C++。
#
# REQUIRES 决定哪些 IDF 库进入最终链接行，Rust 侧 extern 声明的符号由它们提供:
#   mini_tree         —— device/VFS、任务管理、延时、ERR_PTR 编码
#   driver_ws2812     —— 板载 RGB 驱动 (ioctl 命令号)
#   espressif__cjson  —— cJSON_Parse 等
#   log               —— esp_log_write (mini_tree 的 CONFIG_SYS_LOG_USE_ESP=y 后端)
#
# 说明：staticlib 只归档不链接，无需 ldproxy / LIBCLANG_PATH；cargo +esp 独立可用。
#       ESP-IDF 依赖解析阶段以「脚本模式」include 本文件，遇 idf_component_register 即 return，
#       故所有 add_custom_command / add_library 等非脚本化命令必须放在 register 之后。

idf_component_register(REQUIRES mini_tree driver_ws2812 espressif__cjson log)

find_program(CARGO_EXE cargo
    HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXE)
    message(FATAL_ERROR "cargo not found; 请安装 Rust 并确保 ~/.cargo/bin 可被定位（espup install 后）")
endif()

set(RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target/xtensa-esp32s3-none-elf/release")
set(RUST_LIB "${RUST_TARGET_DIR}/libapp_rust.a")

# 构建期调用 cargo 交叉编译 Rust 静态库；ALL 目标每次都调，由 cargo 自身判断增量新鲜度
add_custom_command(
    OUTPUT "${RUST_LIB}"
    COMMAND "${CARGO_EXE}" +esp build --release
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building app_rust staticlib (cargo +esp, xtensa-esp32s3-none-elf)"
    VERBATIM
)
add_custom_target(app_rust_cargo ALL DEPENDS "${RUST_LIB}")

# 导入预编译静态库并链入本组件（INTERFACE 传递给依赖方，最终进固件链接行）
add_library(app_rust_lib STATIC IMPORTED GLOBAL)
set_target_properties(app_rust_lib PROPERTIES IMPORTED_LOCATION "${RUST_LIB}")
add_dependencies(app_rust_lib app_rust_cargo)
target_link_libraries(${COMPONENT_LIB} INTERFACE app_rust_lib)

# 链接顺序说明（关键，勿删）:
#
# IDF 5.5 的最终链接行**没有** --start-group/--end-group，静态库按拓扑序单向扫描。
# app_rust 依赖 cjson / mini_tree / log，拓扑展开时 libapp_rust.a 必然排在这些库
# **之后**; 而 ld 扫描 cjson.a 时 Rust 的引用尚未出现，该库成员不会被拉入; 等扫到
# Rust 库再引用 cJSON_Parse / device_ioctl / esp_log_write 时已无库可提供，
# 报 undefined reference。
#
# 已验证不可行的做法:
#   - target_link_libraries(BEFORE INTERFACE ...) —— CMake 报 "INTERFACE library can
#     only be used with the INTERFACE keyword"（app_rust_lib 是 IMPORTED 库）;
#   - $<LINK_GROUP:RESCAN,...> —— IDF 5.5 在 component.cmake / project.cmake 里自行
#     遍历 INTERFACE_LINK_LIBRARIES 做分类，无法处理该 genexpr，generate 阶段报错。
#
# 生效做法: 在 main/CMakeLists.txt 里把 app_rust_lib PREPEND 到 main 组件的库列表，
# 使其位于所有依赖之前 —— 「Rust 先扫描、依赖后提供」，方向正确。