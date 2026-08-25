#pragma once
#include "cJSON.h"
#include "cJSON.h"
#include "compiler_compat.h"
#include "etl/nullptr.h"
#include "etl/optional.h"
#include "osal.h"
#include "task_manager.hpp"
#include "status.h"
#include "system_log.h"
namespace cjson_test 
{
    etl::optional<int> app_cjson_task_entry();
}