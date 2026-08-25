#include "start.hpp"
#include "compiler_compat.h"
extern "C" void app_main(void)
{
    COMPAT_IGNORE_RESULT(start());
}
