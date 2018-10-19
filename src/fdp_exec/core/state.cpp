#include "state.hpp"

#define PRIVATE_CORE__
#define FDP_MODULE "state"
#include "log.hpp"
#include "utils.hpp"
#include "core.hpp"
#include "private.hpp"
#include "os.hpp"

#include <FDP.h>

#include <map>
#include <thread>

namespace
{
    struct Breakpoint
    {
        opt<uint64_t> dtb;
        int           id;
    };

    struct BreakpointObserver
    {
        BreakpointObserver(const core::Task& task, uint64_t phy, proc_t proc, core::filter_e filter)
            : task(task)
            , phy(phy)
            , proc(proc)
            , filter(filter)
            , bpid(-1)
        {
        }

        core::Task      task;
        uint64_t        phy;
        proc_t          proc;
        core::filter_e  filter;
        int             bpid;

    };

    using Observer = std::shared_ptr<BreakpointObserver>;

    using Breakpoints = struct
    {
        std::unordered_map<uint64_t, Breakpoint>    targets_;
        std::multimap<uint64_t, Observer>           observers_;
    };
}

struct core::State::Data
{
    Data(FDP_SHM& shm, Core& core)
        : shm(shm)
        , core(core)
    {
    }

    FDP_SHM&    shm;
    Core&       core;
    Breakpoints breakpoints;
};

core::State::State()
{
}

core::State::~State()
{
}

void core::setup(State& mem, FDP_SHM& shm, Core& core)
{
    mem.d_ = std::make_unique<core::State::Data>(shm, core);
}

namespace
{
    bool update_break_state(core::State::Data& d)
    {
        const auto current = d.core.os->get_current_proc();
        if(!current)
            FAIL(false, "unable to get current process & update break state");

        d.core.mem.update(*current);
        return true;
    }
}

bool core::State::pause()
{
    const auto ok = FDP_Pause(&d_->shm);
    if(!ok)
        FAIL(false, "unable to pause");

    const auto updated = update_break_state(*d_);
    return updated;
}

bool core::State::resume()
{
    auto& shm = d_->shm;
    FDP_State state = FDP_STATE_NULL;
    auto ok = FDP_GetState(&shm, &state);
    if(ok)
        if(state & FDP_STATE_BREAKPOINT_HIT)
            FDP_SingleStep(&shm, 0);

    ok = FDP_Resume(&shm);
    if(!ok)
        FAIL(false, "unable to resume");

    return true;
}

struct core::BreakpointPrivate
{
    BreakpointPrivate(core::State::Data& core, const Observer& observer)
        : core_(core)
        , observer_(observer)
    {
    }

    ~BreakpointPrivate()
    {
        utils::erase_if(core_.breakpoints.observers_, [&](auto bp)
        {
            return observer_ == bp;
        });
        const auto range = core_.breakpoints.observers_.equal_range(observer_->phy);
        const auto empty = range.first == range.second;
        if(!empty)
            return;

        const auto ok = FDP_UnsetBreakpoint(&core_.shm, static_cast<uint8_t>(observer_->bpid));
        if(!ok)
            LOG(ERROR, "unable to remove breakpoint %d", observer_->bpid);

        core_.breakpoints.targets_.erase(observer_->phy);
    }

    core::State::Data&  core_;
    Observer            observer_;
};

namespace
{
    void check_breakpoints(core::State::Data& d, FDP_State state)
    {
        if(!(state & FDP_STATE_BREAKPOINT_HIT))
            return;

        if(d.breakpoints.observers_.empty())
            return;

        const auto rip = d.core.regs.read(FDP_RIP_REGISTER);
        if(!rip)
            return;

        const auto cr3 = d.core.regs.read(FDP_CR3_REGISTER);
        if(!cr3)
            return;

        uint64_t phy = 0;
        const auto ok = FDP_VirtualToPhysical(&d.shm, 0, *rip, &phy);
        if(!ok)
            return;

        const auto range = d.breakpoints.observers_.equal_range(phy);
        for(auto it = range.first; it != range.second; ++it)
        {
            const auto& bp = *it->second;
            if(bp.filter == core::FILTER_CR3 && bp.proc.dtb != *cr3)
                continue;

            bp.task();
        }
    }
}

bool core::State::wait()
{
    auto& shm = d_->shm;
    while(true)
    {
        std::this_thread::yield();
        auto ok = FDP_GetStateChanged(&shm);
        if(!ok)
            continue;

        update_break_state(*d_);
        FDP_State state = FDP_STATE_NULL;
        ok = FDP_GetState(&shm, &state);
        if(!ok)
            return false;

        check_breakpoints(*d_, state);
        return true;
    }
}

namespace
{
    int try_add_breakpoint(core::State::Data& d, uint64_t phy, const BreakpointObserver& bp)
    {
        auto& targets = d.breakpoints.targets_;
        auto dtb = bp.filter == core::FILTER_CR3 ? std::make_optional(bp.proc.dtb) : std::nullopt;
        const auto it = targets.find(phy);
        if(it != targets.end())
        {
            // keep using found breakpoint if filtering rules are compatible
            const auto bp_dtb = it->second.dtb;
            if(!bp_dtb || bp_dtb == dtb)
                return it->second.id;

            // filtering rules are too restrictive, remove old breakpoint & add an unfiltered breakpoint
            const auto ok = FDP_UnsetBreakpoint(&d.shm, static_cast<uint8_t>(it->second.id));
            targets.erase(it);
            if(!ok)
                return -1;

            // add new breakpoint without filtering
            dtb = std::nullopt;
        }

        const auto bpid = FDP_SetBreakpoint(&d.shm, 0, FDP_SOFTHBP, 0, FDP_EXECUTE_BP, FDP_PHYSICAL_ADDRESS, phy, 1, dtb ? *dtb : FDP_NO_CR3);
        if(bpid < 0)
            return -1;

        targets.emplace(phy, Breakpoint{dtb, bpid});
        return bpid;
    }
}

core::Breakpoint core::State::set_breakpoint(uint64_t ptr, proc_t proc, core::filter_e filter, const core::Task& task)
{
    const auto phy = d_->core.mem.virtual_to_physical(ptr, proc.dtb);
    if(!phy)
        return nullptr;

    const auto bp = std::make_shared<BreakpointObserver>(task, *phy, proc, filter);
    d_->breakpoints.observers_.emplace(*phy, bp);
    const auto bpid = try_add_breakpoint(*d_, *phy, *bp);

    // update all observers breakpoint id
    const auto range = d_->breakpoints.observers_.equal_range(*phy);
    for(auto it = range.first; it != range.second; ++it)
        it->second->bpid = bpid;

    return std::make_shared<core::BreakpointPrivate>(*d_, bp);
}