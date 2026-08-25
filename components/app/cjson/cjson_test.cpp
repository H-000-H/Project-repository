#include "cjson_test.hpp"
#include "cJSON.h"
#include "osal.h"
#include "system_log.h"
constexpr auto kTask_Stack_Size = 4096;
constexpr auto kTask_Priority = 5;
constexpr auto kTask_Core_Id = -1;
constexpr auto kTask_Name = "cjson_test_task";
namespace cjson_test
{
    const char* json = "{\"name\":\"esp32s3\",\"id\":42,\"on\":true}";


    void entry(void* param)
    {
        for(;;)
        {
            cJSON* root = cJSON_Parse(json);
            if(root == nullptr)
            {
                const char* err = cJSON_GetErrorPtr();
                SYS_LOGE(kTask_Name, "cJSON_Parse failed: %s", err ? err : "unknown");
                break;
            }

            cJSON* name = cJSON_GetObjectItem(root, "name");
            cJSON* id   = cJSON_GetObjectItem(root, "id");
            cJSON* judgment = cJSON_GetObjectItem(root,"on");
            if(cJSON_IsString(name) && (name->valuestring != nullptr))
            {
                SYS_LOGI(kTask_Name, "name=%s", name->valuestring);
                osal_delay_ms(1000);
                SYS_LOGE(kTask_Name,"judgment,%d",judgment->valueint);
                if(cJSON_IsNumber(id))
                    SYS_LOGW(kTask_Name, "id=%d", id->valueint);   /* id 是数字, 用 valueint */
            }
            cJSON_Delete(root);
            osal_delay_ms(500);
        }
    }

    etl::optional<int> app_cjson_task_entry()
    {
        if(TaskManager::create_task(kTask_Name, kTask_Stack_Size, kTask_Priority, entry, nullptr, kTask_Core_Id)!=VFS_OK)
            return etl::make_optional(VFS_ERR_NODEV);
        return etl::make_optional(VFS_OK);
    }
}