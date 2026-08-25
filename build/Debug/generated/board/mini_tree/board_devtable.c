#include "board_nodes.h"
#include "board_devtable.h"
#include "device.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ===== 属性表 (静态 .rodata) ===== */

/* ===== 依赖表 ===== */

/* ===== reg 分组表 ===== */

/* ===== irq 表 ===== */

/* ===== 主节点表 ===== */
static const struct device_node s_nodes[DEV_ID_COUNT] = {
    [DEV_ID_] = {
        .name       = "",
        .label      = "",
        .compatible = "wch,ch32v307-node",
        .path       = "/",
        .status     = DEVICE_STATUS_READY,
        .criticality = DEVICE_CRIT_WARNING,
        .flags      = 0,
        .prop_count = 0,
        .props      = NULL,
        .dep_count  = 0,
        .deps       = (const device_id_t*)NULL,
        .reg_count  = 0,
        .regs       = (const struct device_reg*)NULL,
        .irq_count  = 0,
        .irqs       = (const struct device_irq*)NULL,
    },
};

const struct device_node* board_node_get(device_id_t id) {
    if ((int)id < 0 || (int)id >= DEV_ID_COUNT) return NULL;
    return &s_nodes[id];
}

int board_dev_count(void) { return DEV_ID_COUNT; }

device_id_t board_dev_find(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < DEV_ID_COUNT; i++) {
        if (strcmp(s_nodes[i].name, name) == 0)
            return (device_id_t)i;
    }
    return -1;
}

device_id_t board_dev_find_by_compat(const char* compatible) {
    if (!compatible) return -1;
    for (int i = 0; i < DEV_ID_COUNT; i++) {
        if (s_nodes[i].compatible[0] &&
            strcmp(s_nodes[i].compatible, compatible) == 0)
            return (device_id_t)i;
    }
    return -1;
}

device_id_t board_dev_find_by_label(const char* label) {
    if (!label || !label[0]) return -1;
    for (int i = 0; i < DEV_ID_COUNT; i++) {
        if (s_nodes[i].label[0] &&
            strcmp(s_nodes[i].label, label) == 0)
            return (device_id_t)i;
    }
    return -1;
}
