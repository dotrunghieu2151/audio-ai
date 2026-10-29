
#include "app-window.h"
#include "dth/common/timer.hpp"
#include "spdlog/spdlog.h"
int main(int argc, char** argv)
{
    dth::common::Timer timer{};
    {
        auto ui = AppWindow::create();

        ui->on_request_increase_value([&] { ui->set_counter(ui->get_counter() + 1); });

        ui->run();
    }  // ui is destroyed here
    auto timed = timer.stop_ms();
    spdlog::info("Hello, Worldi! {}", timed);
}
