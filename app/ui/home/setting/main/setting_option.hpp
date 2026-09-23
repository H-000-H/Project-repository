#pragma once
#include "lvgl.h"
namespace ui
{
    class Option
    {
    public:
        Option();
        ~Option();

    protected:
        bool create_option(lv_obj_t*parent);
        void set_option_size();
        void set_option_color();
    };
}
