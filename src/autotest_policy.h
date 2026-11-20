#pragma once

#include <cstdint>
#include <string>

namespace G3O {

constexpr int kAutoTestExitSuccess = 0;
constexpr int kAutoTestExitLoadFailed = 10;
constexpr int kAutoTestExitHookUnavailable = 11;
constexpr int kAutoTestExitInternalTimeout = 12;
constexpr int kAutoTestExitEngineFatal = 20;
constexpr int kAutoTestExitUnhandledException = 21;

enum class AutoTestState : std::uint8_t {
    Disabled = 0,
    WaitingForMainMenu = 1,
    MenuSettling = 2,
    Loading = 3,
    PostLoadSettling = 4,
    Finishing = 5,
    Done = 6,
    Aborted = 7
};

enum class AutoTestAction : std::uint8_t {
    None = 0,
    TriggerLoad = 1,
    FlushAndExit = 2
};

struct AutoTestPolicyConfig {
    bool enabled = false;
    std::string slot = "Autosave_Slot_0001";
    int targetLoads = 2;
    int settleFrames = 60;
    int menuSettleTicks = 30;
    int timeoutSec = 180;
    int menuTimeoutSec = 60;
    int loadTimeoutSec = 120;
};

class AutoTestStateMachine {
public:
    void Start(const AutoTestPolicyConfig& cfg, std::uint64_t startTimeMs) {
        m_cfg = cfg;
        if (m_cfg.targetLoads < 1) m_cfg.targetLoads = 1;
        if (m_cfg.settleFrames < 1) m_cfg.settleFrames = 1;
        if (m_cfg.menuSettleTicks < 1) m_cfg.menuSettleTicks = 1;
        if (m_cfg.timeoutSec < 30) m_cfg.timeoutSec = 30;
        if (m_cfg.menuTimeoutSec <= 0 || m_cfg.menuTimeoutSec > m_cfg.timeoutSec) {
            m_cfg.menuTimeoutSec = m_cfg.timeoutSec < 60 ? m_cfg.timeoutSec : 60;
        }
        if (m_cfg.loadTimeoutSec <= 0 || m_cfg.loadTimeoutSec > m_cfg.timeoutSec) {
            m_cfg.loadTimeoutSec = m_cfg.timeoutSec < 120 ? m_cfg.timeoutSec : 120;
        }

        m_startTimeMs = startTimeMs;
        m_phaseStartMs = startTimeMs;
        m_loadsCompleted = 0;
        m_currentLoadIndex = 0;
        m_settleTicksRemaining = 0;
        m_exitCode = kAutoTestExitSuccess;
        m_abortReason.clear();

        if (!m_cfg.enabled) {
            m_state = AutoTestState::Disabled;
            return;
        }
        if (m_cfg.slot.empty()) {
            Abort(kAutoTestExitLoadFailed, "empty_slot_name");
            return;
        }
        m_state = AutoTestState::WaitingForMainMenu;
    }

    AutoTestAction AbortHookUnavailable(const char* detail = "hook_unavailable") {
        if (IsTerminal()) return AutoTestAction::None;
        Abort(kAutoTestExitHookUnavailable, detail ? detail : "hook_unavailable");
        return AutoTestAction::FlushAndExit;
    }

    AutoTestAction OnMainLoopTick(
        bool isMenuReady, bool isLoadActive, bool isGameplayReady, std::uint64_t nowMs) {
        if (m_state == AutoTestState::Disabled || IsTerminal()) {
            return AutoTestAction::None;
        }

        if (CheckTimeouts(nowMs)) {
            return AutoTestAction::FlushAndExit;
        }

        if (isLoadActive || m_state == AutoTestState::Loading) {
            return AutoTestAction::None;
        }

        switch (m_state) {
        case AutoTestState::WaitingForMainMenu:
            if (isMenuReady) {
                m_state = AutoTestState::MenuSettling;
                m_phaseStartMs = nowMs;
                m_settleTicksRemaining = m_cfg.menuSettleTicks;
            }
            return AutoTestAction::None;

        case AutoTestState::MenuSettling:
            if (!isMenuReady) {
                m_state = AutoTestState::WaitingForMainMenu;
                return AutoTestAction::None;
            }
            if (m_settleTicksRemaining > 0) {
                --m_settleTicksRemaining;
            }
            if (m_settleTicksRemaining == 0) {
                m_currentLoadIndex = m_loadsCompleted + 1;
                m_state = AutoTestState::Loading;
                m_phaseStartMs = nowMs;
                return AutoTestAction::TriggerLoad;
            }
            return AutoTestAction::None;

        case AutoTestState::PostLoadSettling:
            if (!isGameplayReady) {
                return AutoTestAction::None;
            }
            if (m_settleTicksRemaining > 0) {
                --m_settleTicksRemaining;
            }
            if (m_settleTicksRemaining == 0) {
                if (m_loadsCompleted >= m_cfg.targetLoads) {
                    m_state = AutoTestState::Done;
                    m_exitCode = kAutoTestExitSuccess;
                    return AutoTestAction::FlushAndExit;
                }
                m_currentLoadIndex = m_loadsCompleted + 1;
                m_state = AutoTestState::Loading;
                m_phaseStartMs = nowMs;
                return AutoTestAction::TriggerLoad;
            }
            return AutoTestAction::None;

        default:
            return AutoTestAction::None;
        }
    }

    void OnLoadStarted(std::uint64_t nowMs) {
        if (m_state == AutoTestState::Disabled || IsTerminal()) return;
        if (m_state != AutoTestState::Loading) {
            m_currentLoadIndex = m_loadsCompleted + 1;
            m_state = AutoTestState::Loading;
        }
        m_phaseStartMs = nowMs;
    }

    AutoTestAction OnLoadFinished(bool succeeded, double /*durationSec*/, std::uint64_t nowMs) {
        if (m_state == AutoTestState::Disabled || IsTerminal()) {
            return AutoTestAction::None;
        }
        if (!succeeded) {
            Abort(kAutoTestExitLoadFailed, "load_game_returned_false");
            return AutoTestAction::FlushAndExit;
        }
        ++m_loadsCompleted;
        m_state = AutoTestState::PostLoadSettling;
        m_phaseStartMs = nowMs;
        m_settleTicksRemaining = m_cfg.settleFrames;
        return AutoTestAction::None;
    }

    AutoTestAction OnWatchdogTick(std::uint64_t nowMs) {
        if (m_state == AutoTestState::Disabled || IsTerminal()) {
            return AutoTestAction::None;
        }
        if (CheckTimeouts(nowMs)) {
            return AutoTestAction::FlushAndExit;
        }
        return AutoTestAction::None;
    }

    AutoTestState State() const noexcept { return m_state; }
    int ExitCode() const noexcept { return m_exitCode; }
    const std::string& AbortReason() const noexcept { return m_abortReason; }
    int LoadsCompleted() const noexcept { return m_loadsCompleted; }
    int CurrentLoadIndex() const noexcept { return m_currentLoadIndex; }
    int TargetLoads() const noexcept { return m_cfg.targetLoads; }
    const std::string& Slot() const noexcept { return m_cfg.slot; }
    bool IsTerminal() const noexcept {
        return m_state == AutoTestState::Done || m_state == AutoTestState::Aborted;
    }

private:
    static std::uint64_t ElapsedMs(std::uint64_t nowMs, std::uint64_t sinceMs) noexcept {
        return nowMs >= sinceMs ? (nowMs - sinceMs) : 0;
    }

    bool CheckTimeouts(std::uint64_t nowMs) {
        const std::uint64_t totalLimitMs = static_cast<std::uint64_t>(m_cfg.timeoutSec) * 1000ULL;
        if (ElapsedMs(nowMs, m_startTimeMs) >= totalLimitMs) {
            Abort(kAutoTestExitInternalTimeout, "total_timeout");
            return true;
        }
        if (m_state == AutoTestState::WaitingForMainMenu || m_state == AutoTestState::MenuSettling) {
            const std::uint64_t menuLimitMs = static_cast<std::uint64_t>(m_cfg.menuTimeoutSec) * 1000ULL;
            if (ElapsedMs(nowMs, m_startTimeMs) >= menuLimitMs) {
                Abort(kAutoTestExitInternalTimeout, "menu_ready_timeout");
                return true;
            }
        }
        if (m_state == AutoTestState::Loading) {
            const std::uint64_t loadLimitMs = static_cast<std::uint64_t>(m_cfg.loadTimeoutSec) * 1000ULL;
            if (ElapsedMs(nowMs, m_phaseStartMs) >= loadLimitMs) {
                Abort(kAutoTestExitInternalTimeout, "load_timeout");
                return true;
            }
        }
        return false;
    }

    void Abort(int exitCode, const char* reason) {
        m_state = AutoTestState::Aborted;
        m_exitCode = exitCode;
        m_abortReason = reason ? reason : "unknown";
    }

    AutoTestPolicyConfig m_cfg{};
    AutoTestState m_state = AutoTestState::Disabled;
    std::uint64_t m_startTimeMs = 0;
    std::uint64_t m_phaseStartMs = 0;
    int m_loadsCompleted = 0;
    int m_currentLoadIndex = 0;
    int m_settleTicksRemaining = 0;
    int m_exitCode = kAutoTestExitSuccess;
    std::string m_abortReason;
};

} // namespace G3O
