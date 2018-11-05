#include "core.hpp"
#include "callstack.hpp"

#define FDP_MODULE "main"
#include "log.hpp"
#include "os.hpp"
#include "utils/pe.hpp"
#include "utils/sanitizer.hpp"

#include <thread>
#include <chrono>

namespace
{
    bool test_core(core::Core& core, pe::Pe& pe)
    {
        LOG(INFO, "drivers:");
        core.os->driver_list([&](driver_t drv)
        {
            const auto name = core.os->driver_name(drv);
            const auto span = core.os->driver_span(drv);
            LOG(INFO, "    driver: %" PRIx64 " %s 0x%" PRIx64 " 0x%" PRIx64 "", drv.id, name ? name->data() : "<noname>", span ? span->addr : 0, span ? span->size : 0);
            return WALK_NEXT;
        });

        const auto pc = core.os->proc_current();
        LOG(INFO, "current process: %" PRIx64 " dtb: %" PRIx64 " %s", pc->id, pc->dtb, core.os->proc_name(*pc)->data());

        const auto tc = core.os->thread_current();
        LOG(INFO, "current thread: %" PRIx64 "", tc->id);

        LOG(INFO, "processes:");
        core.os->proc_list([&](proc_t proc)
        {
            const auto procname = core.os->proc_name(proc);
            LOG(INFO, "proc: %" PRIx64 " %s", proc.id, procname ? procname->data() : "<noname>");
            return WALK_NEXT;
        });

        const char proc_target[] = "notepad.exe";
        LOG(INFO, "searching %s", proc_target);
        const auto target = core.os->proc_find(proc_target);
        if(!target)
            return false;

        LOG(INFO, "%s: %" PRIx64 " dtb: %" PRIx64 " %s", proc_target, target->id, target->dtb, core.os->proc_name(*target)->data());
        const auto join = core.state.proc_join(*target, core::JOIN_USER_MODE);
        if(!join)
            return false;

        std::vector<uint8_t> buffer;
        size_t modcount = 0;
        core.os->mod_list(*target, [&](mod_t)
        {
            ++modcount;
            return WALK_NEXT;
        });
        size_t modi = 0;
        core.os->mod_list(*target, [&](mod_t mod)
        {
            const auto name = core.os->mod_name(*target, mod);
            const auto span = core.os->mod_span(*target, mod);
            if(!name || !span)
                return WALK_NEXT;

            LOG(INFO, "module[%03zd/%03zd] %s: 0x%" PRIx64 " 0x%zx", modi, modcount, name->data(), span->addr, span->size);
            ++modi;

            const auto debug_dir = pe.get_directory_entry(core, *span, pe::pe_directory_entries_e::IMAGE_DIRECTORY_ENTRY_DEBUG);
            buffer.resize(debug_dir->size);
            auto ok = core.mem.virtual_read(&buffer[0], debug_dir->addr, debug_dir->size);
            if(!ok)
                return WALK_NEXT;

            const auto codeview = pe.parse_debug_dir(&buffer[0], span->addr, *debug_dir);
            buffer.resize(codeview->size);
            ok = core.mem.virtual_read(&buffer[0], codeview->addr, codeview->size);
            if (!ok)
                FAIL(WALK_NEXT, "Unable to read IMAGE_CODEVIEW (RSDS)");

            ok = core.sym.insert(sanitizer::sanitize_filename(*name).data(), *span, &buffer[0], buffer.size());
            if(!ok)
                return WALK_NEXT;

            return WALK_NEXT;
        });

        core.os->thread_list(*target, [&](thread_t thread)
        {
            const auto rip = core.os->thread_pc(*target, thread);
            if(!rip)
                return WALK_NEXT;

            const auto name = core.sym.find(*rip);
            LOG(INFO, "thread: %" PRIx64 " 0x%" PRIx64 "%s", thread.id, *rip, name ? (" " + name->module + "!" + name->symbol + "+" + std::to_string(name->offset)).data() : "");
            return WALK_NEXT;
        });

        // check breakpoints
        {
            const auto ptr = core.sym.symbol("nt", "SwapContext");
            const auto bp = core.state.set_breakpoint(*ptr, *target, core::ANY_CR3, [&]
            {
                const auto rip = core.regs.read(FDP_RIP_REGISTER);
                if(!rip)
                    return;

                const auto proc = core.os->proc_current();
                const auto pid = core.os->proc_id(*proc);
                const auto thread = core.os->thread_current();
                const auto tid = core.os->thread_id(*proc, *thread);
                const auto procname = proc ? core.os->proc_name(*proc) : exp::nullopt;
                const auto sym = core.sym.find(*rip);
                LOG(INFO, "BREAK! rip: %" PRIx64 " %s %s pid:%" PRId64 " tid:%" PRId64,
                    *rip, sym ? sym::to_string(*sym).data() : "", procname ? procname->data() : "", pid, tid);
            });
            for(size_t i = 0; i < 16; ++i)
            {
                core.state.resume();
                core.state.wait();
            }
        }

        const auto callstack = callstack::make_callstack_nt(core, pe);

        // test callstack
        {
            const auto n_trigger_bp = 3;
            const auto cs_depth = 40;

            const auto pdb_name = "ntdll";
            const auto func_name = "RtlAllocateHeap";
            const auto func_addr = core.sym.symbol(pdb_name, func_name);
            LOG(INFO, "%s = 0x%" PRIx64, func_name, func_addr ? *func_addr : 0);
            const auto bp = core.state.set_breakpoint(*func_addr, *target, core::FILTER_CR3);

            for (size_t i = 0; i < n_trigger_bp; ++i){
                core.state.resume();
                core.state.wait();
                const auto rip = core.regs.read(FDP_RIP_REGISTER);
                const auto rsp = core.regs.read(FDP_RSP_REGISTER);
                const auto rbp = core.regs.read(FDP_RBP_REGISTER);
                int k = 0;
                callstack->get_callstack(*target, *rip, *rsp, *rbp, [&](sym::Cursor mc)
                {
                    k++;
                    LOG(INFO, "%" PRId32 " - %s", k, sym::to_string(mc).data());
                    if (k>=cs_depth){
                        return WALK_STOP;
                    }
                    return WALK_NEXT;
                });
                LOG(INFO, "");
            }
        }

        {
            const auto pdb_name = "ntdll";
            const auto my_imod = core.sym.find(pdb_name);
            if (!my_imod)
                FAIL(false, "Unable to find pdb of %s", pdb_name);

            std::map<std::string, uint64_t> nt_symbols;

            my_imod->sym_list([&](std::string name, uint64_t offset)
            {
                if (name.find("Nt") != 0 || name.find("Ntdll") != std::string::npos)
                    return WALK_NEXT;

                nt_symbols.emplace(name, offset);
                LOG(INFO, "FOUND %s - %" PRIx64, name.data(), offset);
                return WALK_NEXT;
            });

            if (nt_symbols.size() == 0)
                FAIL(false, "Found no symbols that contains Nt");

            if (false){
                LOG(INFO, "SYMBOLS");
                for (auto s : nt_symbols)
                    LOG(INFO, "Symbol %" PRIx64 " - %s", s.second, s.first.data());
                LOG(INFO, "END SYMBOLS %" PRIx64, nt_symbols.size());
            }

            std::vector<core::Breakpoint> my_bps;
            my_bps.resize(nt_symbols.size());
            std::unordered_map<uint64_t, std::string> bps;

            for (auto s : nt_symbols){
                my_bps.push_back(core.state.set_breakpoint(s.second, *target, core::FILTER_CR3));
                bps.emplace(s.second, s.first);
            }

            LOG(INFO, "Number of breakpoints %" PRIx64, my_bps.size());
            for(size_t i = 0; i < 600; ++i)
            {
                core.state.resume();
                core.state.wait();

                const auto rip = core.regs.read(FDP_RIP_REGISTER);
                const auto it = bps.find(*rip);
                if (it != bps.end())
                    LOG(INFO, "%" PRIu64 " - Bp %" PRIx64 " - %s", i, it->first, it->second.data());
            }
        }

        return true;
    }
}
int main(int argc, char* argv[])
{
    loguru::g_preamble_uptime = false;
    loguru::g_preamble_date = false;
    loguru::g_preamble_thread = false;
    loguru::g_preamble_file = false;
    loguru::init(argc, argv);
    if(argc != 2)
        FAIL(-1, "usage: fdp_exec <name>");

    const auto name = std::string{argv[1]};
    LOG(INFO, "starting on %s", name.data());

    core::Core core;
    auto ok = core::setup(core, name);
    if(!ok)
        FAIL(-1, "unable to start core at %s", name.data());

    pe::Pe pe;
    ok = pe.setup(core);
    if(!ok)
        FAIL(-1, "unable retreive PE format informations from pdb");

    //core.state.resume();
    core.state.pause();
    const auto valid = test_core(core, pe);
    core.state.resume();
    return !valid;
}
