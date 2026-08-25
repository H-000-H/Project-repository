/* SPDX-License-Identifier: Apache-2.0 */
#include "start.hpp"
#include "ws2812.hpp"
#include "driver.h"
#include "system_init.h"
#include "system_log.h"
#include "etl/optional.h"
#include "etl/string_view.h"
#include "cjson_test.hpp"
namespace app::start
{
constexpr etl::string_view kTag{"AppStart"};
} // namespace app::start

etl::optional<int> start()
{
    using app::start::kTag;

    mini_tree_pre_os_init();
    board_register_all_drivers();
    mini_tree_start_tasks();

    const etl::optional<int> led = app_ws2812_task_start();
    if (!led)
    {
        SYS_LOGW(kTag.data(), "ws2812 task not started");
        system_init_complete();
        return etl::nullopt;
    }

    const etl::optional<int> cjson = cjson_test::app_cjson_task_entry();
    if(!cjson)
    {
        SYS_LOGW(kTag.data(), "cjson_task task not started"); 
        system_init_complete();
        return etl::nullopt;
    }

    system_init_complete();
    SYS_LOGI(kTag.data(), "app start complete");
    return 0;
}
