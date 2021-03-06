#define FDP_MODULE "main"
#include <icebox/core.hpp>
#include <icebox/log.hpp>
#include <icebox/plugins/heapsan.hpp>

namespace
{
    int heapsan(core::Core& core, const std::string& target)
    {
        LOG(INFO, "waiting for %s...", target.data());
        const auto proc = process::wait(core, target, flags::x64);
        if(!proc)
            return FAIL(-1, "unable to wait for %s", target.data());

        LOG(INFO, "process %s active", target.data());
        const auto ok = symbols::load_module(core, *proc, "ntdll");
        if(!ok)
            return FAIL(-1, "unable to load ntdll symbols");

        LOG(INFO, "listening events...");
        const auto fdpsan = plugins::HeapSan{core, *proc};
        const auto now    = std::chrono::high_resolution_clock::now();
        const auto end    = now + std::chrono::minutes(5);
        while(std::chrono::high_resolution_clock::now() < end)
            state::exec(core);

        return 0;
    }
}

int main(int argc, char** argv)
{
    logg::init(argc, argv);
    if(argc != 3)
        return FAIL(-1, "usage: nt_writefile <name> <process_name>");

    const auto name   = std::string{argv[1]};
    const auto target = std::string{argv[2]};
    LOG(INFO, "starting on %s", name.data());

    const auto core = core::attach(name);
    if(!core)
        return FAIL(-1, "unable to start core at %s", name.data());

    state::pause(*core);
    const auto ret = heapsan(*core, target);
    state::resume(*core);
    return ret;
}
