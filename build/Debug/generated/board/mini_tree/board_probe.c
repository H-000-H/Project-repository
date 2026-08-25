#include "board_nodes.h"
#include "board_devtable.h"
#include "device.h"

/* ===== probe 函数声明 ===== */

/* ===== remove 函数声明 ===== */

static const probe_fn_t s_probe_fns[DEV_ID_COUNT] = {
    [DEV_ID_] = NULL,
};

static const remove_fn_t s_remove_fns[DEV_ID_COUNT] = {
    [DEV_ID_] = NULL,
};

static const device_id_t s_probe_order[DEV_ID_COUNT] = {
    DEV_ID_,
};

probe_fn_t board_probe_get_fn(device_id_t id) {
    if ((int)id < 0 || (int)id >= DEV_ID_COUNT) return NULL;
    return s_probe_fns[id];
}

remove_fn_t board_remove_get_fn(device_id_t id) {
    if ((int)id < 0 || (int)id >= DEV_ID_COUNT) return NULL;
    return s_remove_fns[id];
}

const device_id_t* board_probe_order(void) {
    return s_probe_order;
}

int board_probe_order_count(void) {
    return DEV_ID_COUNT;
}

static const device_id_t s_cascade_data[] = {
    0,
};

static const uint8_t s_cascade_counts[DEV_ID_COUNT] = {
    [DEV_ID_] = 0,
};

static const uint16_t s_cascade_offset[DEV_ID_COUNT] = {
    [DEV_ID_] = 0,
};

const device_id_t* board_cascade_get(device_id_t id, int* count) {
    if ((int)id < 0 || (int)id >= DEV_ID_COUNT) { *count = 0; return NULL; }
    *count = s_cascade_counts[id];
    return *count ? &s_cascade_data[s_cascade_offset[id]] : NULL;
}

static const device_id_t s_children_data[] = {
    0,
};

static const uint8_t s_children_counts[DEV_ID_COUNT] = {
    [DEV_ID_] = 0,
};

static const uint16_t s_children_offset[DEV_ID_COUNT] = {
    [DEV_ID_] = 0,
};

const device_id_t* board_children_get(device_id_t id, int* count) {
    if ((int)id < 0 || (int)id >= DEV_ID_COUNT) { *count = 0; return NULL; }
    *count = s_children_counts[id];
    return *count ? &s_children_data[s_children_offset[id]] : NULL;
}
