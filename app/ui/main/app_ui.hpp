#ifndef APP_UI_HPP
#define APP_UI_HPP
#include "lvgl_port.hpp"
namespace app 
{
    class Ui
    {
    public:
        static Ui& GetInstance();
        int Init();
        static void Thread(void* param);
        bool ThreadRegister(void);
        private:
        Ui() = default;
        ~Ui() = default;
        Ui(const Ui&) = delete;
        Ui& operator=(const Ui&) = delete;
        Ui(Ui&&) = delete;
        Ui& operator=(Ui&&) = delete;
    };
}
#endif /* APP_UI_HPP */