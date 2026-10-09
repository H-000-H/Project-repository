idf_component_register(WHOLE_ARCHIVE
                    SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES app_rust)

# Rust 静态库的链接顺序（关键，详见 components/app_rust/CMakeLists.txt 末尾说明）:
# app_rust 依赖 cjson / mini_tree / log，拓扑展开会把 libapp_rust.a 排到这些库之后，
# 而 IDF 5.5 的链接行没有 --start-group，ld 单遍扫描解析不到 Rust 引用的 C 符号。
# 这里把 app_rust_lib 提到 main 组件库列表的最前，使其先于所有依赖被扫描。
set_property(TARGET ${COMPONENT_LIB} PREPEND PROPERTY INTERFACE_LINK_LIBRARIES app_rust_lib)