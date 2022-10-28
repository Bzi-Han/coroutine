#include "coroutine.h"

#include <iostream>
#include <string_view>

using namespace coroutine;

task<void, uint32_t> fakeGenerator(size_t count = 10)
{
    srand(static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count()));

    for (size_t i = 0; i < count; i++)
        co_yield rand();
}

task<bool> shutdown()
{
    std::cout << "[=] shutdown" << std::endl;
    co_return true;
}

task<> finally()
{
    co_finally
    {
        bool result = co_await shutdown();

        std::cout << (result ? "[+] " : "[-] ");
        std::cout << "finally block" << std::endl;
    };

    std::cout << "[=] finally" << std::endl;
    co_return;
}

task<> delay()
{
    for (size_t i = 0; i < 3; i++)
    {
        std::cout << "[=] delay" << std::endl;
        co_delay 1000;
    }
}

task<> switchST(const std::string_view &name, size_t count = 0xFFFFFFFF)
{
    for (size_t i = 0; i < count; i++)
    {
        std::cout << "[=] " << name << std::endl;
        co_switch;
    }
}

task<> switchMT(const std::string_view &name, size_t interval = 1000)
{
    for (;;)
    {
        std::cout << "[=] " << name << std::endl;
        co_delay interval;
    }
}

int main()
{
    // manual
    {
        auto coro01 = shutdown();

        while (coro01())
            ;

        std::cout << std::boolalpha;
        std::cout << "[+] result: " << static_cast<bool>(coro01) << " " << coro01.result() << std::endl;
    }

    std::cout << "============================================================" << std::endl;

    // single thread scheduler
    {
        auto coro01 = fakeGenerator(10);

        STScheduler::add(coro01);
        STScheduler::run();

        std::cout << "[=] generate: ";
        for (auto &v : coro01)
            std::cout << v << " ";
        std::cout << std::endl;
    }

    // single thread scheduler
    {
        STScheduler::runs(finally);
    }

    // single thread scheduler
    {
        STScheduler::runs(delay);
    }

    // single thread scheduler
    {
        auto coro01 = switchST("swtich01", 5);
        auto coro02 = switchST("swtich02", 5);

        STScheduler::add(coro01);
        STScheduler::add(coro02);
        STScheduler::run();
    }

    std::cout << "============================================================" << std::endl;

    // multi thread scheduler
    {
        auto coro01 = switchMT("swtich01");
        auto coro02 = switchMT("swtich02", 2000);

        scheduler::add(coro01);
        scheduler::add(coro02);
        scheduler::run();

        std::cout << "press any key to stop running..." << std::endl;
        getchar();

        scheduler::stop();
    }

    return 0;
}