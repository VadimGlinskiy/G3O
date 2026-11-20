#include "autotest_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

static bool TestNormalMultiLoad(int targetLoads) {
    bool ok = true;
    G3O::AutoTestPolicyConfig cfg{};
    cfg.enabled = true;
    cfg.slot = "Autosave_Slot_0001";
    cfg.targetLoads = targetLoads;
    cfg.menuSettleTicks = 3;
    cfg.settleFrames = 4;
    cfg.timeoutSec = 180;

    G3O::AutoTestStateMachine sm;
    sm.Start(cfg, 1000);
    ok &= Expect(sm.State() == G3O::AutoTestState::WaitingForMainMenu, "should start waiting for main menu");

    // Not ready yet
    ok &= Expect(sm.OnMainLoopTick(false, false, false, 1500) == G3O::AutoTestAction::None, "no action before menu ready");

    // Menu becomes ready -> enters MenuSettling
    ok &= Expect(sm.OnMainLoopTick(true, false, false, 2000) == G3O::AutoTestAction::None, "enters MenuSettling");
    ok &= Expect(sm.State() == G3O::AutoTestState::MenuSettling, "state should be MenuSettling");
    ok &= Expect(sm.OnMainLoopTick(true, false, false, 2016) == G3O::AutoTestAction::None, "tick 1 of 3");
    ok &= Expect(sm.OnMainLoopTick(true, false, false, 2032) == G3O::AutoTestAction::None, "tick 2 of 3");
    ok &= Expect(sm.OnMainLoopTick(true, false, false, 2048) == G3O::AutoTestAction::TriggerLoad, "tick 3 triggers Load #1");
    ok &= Expect(sm.State() == G3O::AutoTestState::Loading && sm.CurrentLoadIndex() == 1, "should be in Loading #1");

    // Re-entrancy guard during loading
    ok &= Expect(sm.OnMainLoopTick(true, true, false, 2100) == G3O::AutoTestAction::None, "no action while load is active");

    std::uint64_t now = 5000;
    for (int load = 1; load <= targetLoads; ++load) {
        sm.OnLoadStarted(now);
        now += 10000;
        ok &= Expect(sm.OnLoadFinished(true, 10.0, now) == G3O::AutoTestAction::None, "load finished enters PostLoadSettling");
        ok &= Expect(sm.State() == G3O::AutoTestState::PostLoadSettling && sm.LoadsCompleted() == load, "loadsCompleted incremented");

        // Must not decrement while gameplay is not ready
        ok &= Expect(sm.OnMainLoopTick(false, false, false, now + 16) == G3O::AutoTestAction::None, "wait for gameplay ready");
        for (int f = 1; f < cfg.settleFrames; ++f) {
            now += 16;
            ok &= Expect(sm.OnMainLoopTick(false, false, true, now) == G3O::AutoTestAction::None, "settling frame");
        }
        now += 16;
        const auto action = sm.OnMainLoopTick(false, false, true, now);
        if (load < targetLoads) {
            ok &= Expect(action == G3O::AutoTestAction::TriggerLoad, "triggers next load");
            ok &= Expect(sm.CurrentLoadIndex() == load + 1, "currentLoadIndex incremented");
        } else {
            ok &= Expect(action == G3O::AutoTestAction::FlushAndExit, "final settle triggers FlushAndExit");
            ok &= Expect(sm.State() == G3O::AutoTestState::Done && sm.ExitCode() == G3O::kAutoTestExitSuccess, "done with exit 0");
        }
    }
    return ok;
}

int main() {
    bool ok = true;

    // 1. Disabled mode
    {
        G3O::AutoTestPolicyConfig cfg{};
        cfg.enabled = false;
        G3O::AutoTestStateMachine sm;
        sm.Start(cfg, 0);
        ok &= Expect(sm.State() == G3O::AutoTestState::Disabled, "disabled state");
        ok &= Expect(sm.OnMainLoopTick(true, false, false, 100) == G3O::AutoTestAction::None, "disabled produces no action");
    }

    // 2. Normal 1, 2, 3 load runs
    ok &= TestNormalMultiLoad(1);
    ok &= TestNormalMultiLoad(2);
    ok &= TestNormalMultiLoad(3);

    // 3. Load failure aborts immediately with exit code 10
    {
        G3O::AutoTestPolicyConfig cfg{};
        cfg.enabled = true;
        cfg.targetLoads = 2;
        cfg.menuSettleTicks = 1;
        G3O::AutoTestStateMachine sm;
        sm.Start(cfg, 0);
        sm.OnMainLoopTick(true, false, false, 100);
        ok &= Expect(sm.OnMainLoopTick(true, false, false, 200) == G3O::AutoTestAction::TriggerLoad, "triggers load");
        ok &= Expect(sm.OnLoadFinished(false, 0.1, 300) == G3O::AutoTestAction::FlushAndExit, "failed load exits immediately");
        ok &= Expect(sm.State() == G3O::AutoTestState::Aborted && sm.ExitCode() == G3O::kAutoTestExitLoadFailed, "exit code 10");
    }

    // 4. Menu timeout via watchdog -> exit code 12
    {
        G3O::AutoTestPolicyConfig cfg{};
        cfg.enabled = true;
        cfg.timeoutSec = 180;
        cfg.menuTimeoutSec = 30;
        G3O::AutoTestStateMachine sm;
        sm.Start(cfg, 1000);
        ok &= Expect(sm.OnWatchdogTick(30999) == G3O::AutoTestAction::None, "before menu timeout");
        ok &= Expect(sm.OnWatchdogTick(31000) == G3O::AutoTestAction::FlushAndExit, "at menu timeout");
        ok &= Expect(sm.State() == G3O::AutoTestState::Aborted && sm.ExitCode() == G3O::kAutoTestExitInternalTimeout, "exit code 12");
        ok &= Expect(sm.AbortReason() == "menu_ready_timeout", "menu_ready_timeout reason");
    }

    // 5. Load timeout via watchdog -> exit code 12
    {
        G3O::AutoTestPolicyConfig cfg{};
        cfg.enabled = true;
        cfg.timeoutSec = 180;
        cfg.menuSettleTicks = 1;
        cfg.loadTimeoutSec = 40;
        G3O::AutoTestStateMachine sm;
        sm.Start(cfg, 0);
        sm.OnMainLoopTick(true, false, false, 100);
        sm.OnMainLoopTick(true, false, false, 200); // TriggerLoad at 200
        ok &= Expect(sm.OnWatchdogTick(40199) == G3O::AutoTestAction::None, "before load timeout");
        ok &= Expect(sm.OnWatchdogTick(40200) == G3O::AutoTestAction::FlushAndExit, "at load timeout");
        ok &= Expect(sm.ExitCode() == G3O::kAutoTestExitInternalTimeout && sm.AbortReason() == "load_timeout", "load_timeout reason");
    }

    // 6. Hook unavailable -> exit code 11
    {
        G3O::AutoTestPolicyConfig cfg{};
        cfg.enabled = true;
        G3O::AutoTestStateMachine sm;
        sm.Start(cfg, 0);
        ok &= Expect(sm.AbortHookUnavailable("missing_load_game") == G3O::AutoTestAction::FlushAndExit, "hook unavailable exits");
        ok &= Expect(sm.State() == G3O::AutoTestState::Aborted && sm.ExitCode() == G3O::kAutoTestExitHookUnavailable, "exit code 11");
    }

    if (!ok) return 1;
    std::puts("PASS: autotest_policy state machine, timeouts, and exit codes verified");
    return 0;
}
