#pragma once

// Cao Urban Trainer ASI - Core 2.0 safety and runtime helpers.
// Header-only by design: the current ASI build remains a single translation unit,
// while new systems can use a cleaner shared core API.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "script.h"
#include "natives.h"

namespace cao::core
{
    enum class EntityKind
    {
        Any,
        Ped,
        Vehicle,
        Object
    };

    enum class ErrorSeverity
    {
        Info,
        Warning,
        Error
    };

    enum class ModuleStatus
    {
        Disabled,
        Active,
        Suspended,
        Faulted
    };

    static constexpr size_t kMaxCoreTextBytes = 4096;
    static constexpr size_t kMaxCompatibilityChecks = 256;
    static constexpr size_t kMaxModuleStates = 256;
    static constexpr size_t kMaxRuntimeReports = 512;
    static constexpr DWORD kMaxSafeTickIntervalMs = 0x7FFFFFFFUL;
    static constexpr DWORD kDiagnosticCoalesceWindowMs = 5000UL;

    inline bool IsUtf8ContinuationByte(unsigned char c)
    {
        return (c & 0xC0u) == 0x80u;
    }

    inline size_t Utf8SafePrefixLength(const std::string& value, size_t maxLen)
    {
        if (maxLen >= value.size()) return value.size();
        if (maxLen == 0) return 0;

        // Preserve non-UTF8/ANSI bytes as single-byte text, but avoid cutting a
        // valid UTF-8 multibyte sequence in the middle when the UI/report limit
        // lands inside a Cyrillic or symbol character.
        size_t lead = maxLen - 1;
        while (lead > 0 && IsUtf8ContinuationByte(static_cast<unsigned char>(value[lead])))
            --lead;

        const unsigned char c = static_cast<unsigned char>(value[lead]);
        size_t expected = 1;
        if ((c & 0x80u) == 0x00u) expected = 1;
        else if ((c & 0xE0u) == 0xC0u) expected = 2;
        else if ((c & 0xF0u) == 0xE0u) expected = 3;
        else if ((c & 0xF8u) == 0xF0u) expected = 4;

        const size_t available = maxLen - lead;
        if (expected > 1 && available < expected) return lead;
        return maxLen;
    }

    inline std::string SanitizeCoreText(std::string value, size_t maxLen = 512)
    {
        maxLen = std::min(maxLen, kMaxCoreTextBytes);
        if (maxLen == 0) return std::string();
        for (char& ch : value)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (ch == '\0' || ch == '\r' || ch == '\n' || (c < 32 && ch != '\t')) ch = ' ';
        }
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        if (value.size() > maxLen)
        {
            if (maxLen <= 3)
            {
                value.resize(Utf8SafePrefixLength(value, maxLen));
            }
            else
            {
                const size_t prefixLen = Utf8SafePrefixLength(value, maxLen - 3);
                value = value.substr(0, prefixLen) + "...";
            }
        }
        return value;
    }

    inline void SaturatingIncrement(int& value)
    {
        if (value < std::numeric_limits<int>::max()) ++value;
    }

    enum class CompatibilityLevel
    {
        Ok,
        Info,
        Warning,
        Error
    };

    struct CompatibilityCheck
    {
        std::string id;
        std::string label;
        CompatibilityLevel level = CompatibilityLevel::Ok;
        std::string detail;
    };

    inline const char* CompatibilityLevelName(CompatibilityLevel level)
    {
        switch (level)
        {
            case CompatibilityLevel::Info: return "info";
            case CompatibilityLevel::Warning: return "warning";
            case CompatibilityLevel::Error: return "error";
            case CompatibilityLevel::Ok: default: return "ok";
        }
    }

    class CompatibilityLayer
    {
        std::vector<CompatibilityCheck> checks_;
        bool fallbackActive_ = false;

    public:
        void BeginScan()
        {
            checks_.clear();
            fallbackActive_ = false;
        }

        void Add(const std::string& id, const std::string& label, CompatibilityLevel level, const std::string& detail)
        {
            CompatibilityCheck check;
            check.id = SanitizeCoreText(id.empty() ? "compat.unknown" : id, 96);
            if (check.id.empty()) check.id = "compat.unknown";
            check.label = SanitizeCoreText(label.empty() ? check.id : label, 160);
            if (check.label.empty()) check.label = check.id;
            check.level = level;
            check.detail = SanitizeCoreText(detail.empty() ? CompatibilityLevelName(level) : detail, 512);
            if (check.detail.empty()) check.detail = CompatibilityLevelName(level);
            if (checks_.size() >= kMaxCompatibilityChecks)
                checks_.erase(checks_.begin());
            checks_.push_back(check);
        }

        void SetFallbackActive(bool active) { fallbackActive_ = active; }
        bool FallbackActive() const { return fallbackActive_; }
        const std::vector<CompatibilityCheck>& Checks() const { return checks_; }

        int Count(CompatibilityLevel level) const
        {
            int count = 0;
            for (const CompatibilityCheck& check : checks_) if (check.level == level) ++count;
            return count;
        }

        int WarningCount() const { return Count(CompatibilityLevel::Warning); }
        int ErrorCount() const { return Count(CompatibilityLevel::Error); }
        bool HasErrors() const { return ErrorCount() > 0; }
        bool HasWarnings() const { return WarningCount() > 0; }

        std::string LastProblem() const
        {
            // Prefer the most severe problem. The scan order is informational, so a
            // later warning must not hide an earlier critical error such as missing ScriptHookV.
            for (auto it = checks_.rbegin(); it != checks_.rend(); ++it)
            {
                if (it->level == CompatibilityLevel::Error)
                    return it->label + ": " + it->detail;
            }
            for (auto it = checks_.rbegin(); it != checks_.rend(); ++it)
            {
                if (it->level == CompatibilityLevel::Warning)
                    return it->label + ": " + it->detail;
            }
            return "No compatibility problems";
        }

        std::string Summary() const
        {
            std::ostringstream ss;
            ss << "checks " << checks_.size()
               << " / errors " << ErrorCount()
               << " / warnings " << WarningCount();
            if (fallbackActive_) ss << " / fallback active";
            if (!checks_.empty()) ss << " / last: " << LastProblem();
            return ss.str();
        }
    };

    struct ActionResult
    {
        bool ok = true;
        std::string error;
    };

    struct ErrorEvent
    {
        DWORD tick = 0;
        ErrorSeverity severity = ErrorSeverity::Info;
        std::string module;
        std::string action;
        std::string message;
    };

    struct ModuleState
    {
        std::string id;
        std::string displayName;
        ModuleStatus status = ModuleStatus::Active;
        // Last non-fault state requested by runtime/config refresh. Faulted is a
        // diagnostic overlay, not a desired operating mode; when a bounded error
        // event is cleared/evicted the module should return to this state instead
        // of being blindly revived as Active.
        ModuleStatus expectedStatus = ModuleStatus::Active;
        DWORD lastHeartbeatTick = 0;
        int errorCount = 0;
        int warningCount = 0;
        // True only for explicit runtime faults set through SetStatus/Register.
        // ErrorReporter-driven diagnostics are bounded and can be reconciled away
        // when the event buffer clears; explicit faults must survive that pass until
        // a real recovery/status update clears them.
        bool explicitFault = false;
        std::string lastMessage;
    };

    struct RuntimeListReport
    {
        std::string name;
        EntityKind kind = EntityKind::Any;
        int before = 0;
        int after = 0;
        int removed = 0;
    };

    inline const char* EntityKindName(EntityKind kind)
    {
        switch (kind)
        {
            case EntityKind::Ped: return "ped";
            case EntityKind::Vehicle: return "vehicle";
            case EntityKind::Object: return "object";
            case EntityKind::Any: default: return "entity";
        }
    }

    inline const char* SeverityName(ErrorSeverity severity)
    {
        switch (severity)
        {
            case ErrorSeverity::Warning: return "warning";
            case ErrorSeverity::Error: return "error";
            case ErrorSeverity::Info: default: return "info";
        }
    }

    inline const char* ModuleStatusName(ModuleStatus status)
    {
        switch (status)
        {
            case ModuleStatus::Disabled: return "disabled";
            case ModuleStatus::Suspended: return "suspended";
            case ModuleStatus::Faulted: return "faulted";
            case ModuleStatus::Active: default: return "active";
        }
    }

    class ErrorReporter
    {
        std::vector<ErrorEvent> events_;
        size_t limit_ = 64;
        int infoCount_ = 0;
        int warningCount_ = 0;
        int errorCount_ = 0;

        static int32_t SignedTickDelta(DWORD now, DWORD previous)
        {
            return static_cast<int32_t>(now - previous);
        }

        static bool IsRecentTick(DWORD now, DWORD previous, DWORD windowMs)
        {
            const int32_t signedAge = SignedTickDelta(now, previous);
            if (signedAge < 0) return false;
            return static_cast<DWORD>(signedAge) <= windowMs;
        }

    public:
        explicit ErrorReporter(size_t limit = 64) : limit_(std::min<size_t>(512, limit < 8 ? 8 : limit)) {}

        void Recount()
        {
            infoCount_ = 0;
            warningCount_ = 0;
            errorCount_ = 0;
            for (const ErrorEvent& event : events_)
            {
                if (event.severity == ErrorSeverity::Error) ++errorCount_;
                else if (event.severity == ErrorSeverity::Warning) ++warningCount_;
                else ++infoCount_;
            }
        }

        void Clear()
        {
            events_.clear();
            Recount();
        }

        int ClearWhere(const std::string& module, const std::string& actionPrefix = std::string())
        {
            const std::string safeModule = SanitizeCoreText(module, 96);
            const std::string safeActionPrefix = SanitizeCoreText(actionPrefix, 96);
            // ClearWhere is intended for targeted per-module recovery. An empty
            // module id must not become a broad action-prefix clear across every
            // subsystem; use Clear() explicitly for a full diagnostic reset.
            if (safeModule.empty()) return 0;
            const size_t before = events_.size();
            events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const ErrorEvent& event)
            {
                if (event.module != safeModule) return false;
                if (!safeActionPrefix.empty() && event.action.rfind(safeActionPrefix, 0) != 0) return false;
                return true;
            }), events_.end());
            Recount();
            return static_cast<int>(before - events_.size());
        }

        bool Report(ErrorSeverity severity, const std::string& module, const std::string& action, const std::string& message, DWORD tick = GetTickCount())
        {
            ErrorEvent event;
            event.tick = tick;
            event.severity = severity;
            event.module = SanitizeCoreText(module.empty() ? "core" : module, 96);
            event.action = SanitizeCoreText(action.empty() ? "runtime" : action, 96);
            event.message = SanitizeCoreText(message.empty() ? "No details" : message, 512);
            if (event.module.empty()) event.module = "core";
            if (event.action.empty()) event.action = "runtime";
            if (event.message.empty()) event.message = "No details";

            // Tick-loop warnings can repeat every frame/maintenance pass. Treat an
            // identical recent event as a heartbeat instead of a new diagnostic, even
            // when other subsystem messages are interleaved between repeats. Without
            // this, one stale handle alternating with one native failure can still
            // inflate the Crash Guard counters and repeatedly trigger Recovery Mode.
            for (size_t i = events_.size(); i > 0; --i)
            {
                const size_t index = i - 1;
                ErrorEvent& existing = events_[index];
                const int32_t signedAge = SignedTickDelta(event.tick, existing.tick);
                if (signedAge < 0)
                    continue;
                if (!IsRecentTick(event.tick, existing.tick, kDiagnosticCoalesceWindowMs))
                    continue;
                if (existing.severity == event.severity &&
                    existing.module == event.module &&
                    existing.action == event.action &&
                    existing.message == event.message)
                {
                    existing.tick = event.tick;
                    if (index + 1 != events_.size())
                    {
                        ErrorEvent refreshed = existing;
                        events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(index));
                        events_.push_back(refreshed);
                    }
                    return false;
                }
            }

            if (events_.size() >= limit_)
                events_.erase(events_.begin());
            events_.push_back(event);
            Recount();
            return true;
        }

        bool HasEvents() const { return !events_.empty(); }
        int InfoCount() const { return infoCount_; }
        int WarningCount() const { return warningCount_; }
        int ErrorCount() const { return errorCount_; }
        int TotalCount() const { return infoCount_ + warningCount_ + errorCount_; }
        const std::vector<ErrorEvent>& Events() const { return events_; }

        std::string LastMessage() const
        {
            if (events_.empty()) return "No core errors";
            const ErrorEvent& e = events_.back();
            return std::string("[") + SeverityName(e.severity) + "] " + e.module + ": " + e.action + " - " + e.message;
        }

        std::string LastProblemMessage() const
        {
            if (events_.empty()) return "No core errors";
            for (auto it = events_.rbegin(); it != events_.rend(); ++it)
            {
                if (it->severity == ErrorSeverity::Error)
                    return std::string("[") + SeverityName(it->severity) + "] " + it->module + ": " + it->action + " - " + it->message;
            }
            for (auto it = events_.rbegin(); it != events_.rend(); ++it)
            {
                if (it->severity == ErrorSeverity::Warning)
                    return std::string("[") + SeverityName(it->severity) + "] " + it->module + ": " + it->action + " - " + it->message;
            }
            return LastMessage();
        }

        std::string Summary() const
        {
            std::ostringstream ss;
            ss << "errors " << errorCount_ << ", warnings " << warningCount_;
            if (!events_.empty()) ss << " / problem: " << LastProblemMessage();
            return ss.str();
        }
    };

    class ModuleRegistry
    {
        std::vector<ModuleState> modules_;

        ModuleState* FindMutable(const std::string& id)
        {
            for (ModuleState& module : modules_) if (module.id == id) return &module;
            return nullptr;
        }

        const ModuleState* Find(const std::string& id) const
        {
            for (const ModuleState& module : modules_) if (module.id == id) return &module;
            return nullptr;
        }

        static bool IsDisabledOwned(const ModuleState& module)
        {
            return module.status == ModuleStatus::Disabled || module.expectedStatus == ModuleStatus::Disabled;
        }

        static bool IsSuspendedOwned(const ModuleState& module)
        {
            return module.status == ModuleStatus::Suspended || module.expectedStatus == ModuleStatus::Suspended;
        }

        static bool IsPassiveOwned(const ModuleState& module)
        {
            return IsDisabledOwned(module) || IsSuspendedOwned(module);
        }

        static bool CanDiscoveryStatusReplaceExpected(const ModuleState& module, ModuleStatus discoveredStatus)
        {
            // Register() is called from discovery/init code, not from the runtime
            // config path. Never let discovery downgrade a stronger passive owner:
            // Disabled > Suspended > Active. Intentional runtime changes must go
            // through SetStatus(), where allowFaultRecovery communicates the caller
            // is updating the live state, not just refreshing metadata.
            if (IsDisabledOwned(module))
                return discoveredStatus == ModuleStatus::Disabled;
            if (IsSuspendedOwned(module))
                return discoveredStatus == ModuleStatus::Disabled || discoveredStatus == ModuleStatus::Suspended;
            return true;
        }

        static bool ShouldPreservePassiveExpectedOnStatusRefresh(const ModuleState& module, ModuleStatus requestedStatus, bool allowFaultRecovery)
        {
            // A non-authoritative Active refresh while a passive module is
            // Disabled/Suspended/Faulted must not rewrite expectedStatus. Otherwise
            // a later clear/reconcile pass can revive a disabled/suspended module
            // through stale recovery state.
            return !allowFaultRecovery && requestedStatus == ModuleStatus::Active && IsPassiveOwned(module);
        }

        static ModuleStatus NonFaultExpectedStatus(const ModuleState& module)
        {
            return module.expectedStatus == ModuleStatus::Faulted ? ModuleStatus::Active : module.expectedStatus;
        }

        static ModuleStatus ResolveRecoveredStatus(const ModuleState& module, ModuleStatus recoveredStatus)
        {
            // Disabled and Suspended are runtime/config-owned operating modes.
            // ClearTransientProblems(..., Active) is often used as a generic success
            // state during all-module recovery; it must not wake dormant modules.
            if (IsDisabledOwned(module) && recoveredStatus != ModuleStatus::Disabled)
                return ModuleStatus::Disabled;
            if (IsSuspendedOwned(module) && recoveredStatus == ModuleStatus::Active)
                return ModuleStatus::Suspended;
            return recoveredStatus == ModuleStatus::Faulted ? NonFaultExpectedStatus(module) : recoveredStatus;
        }

        static bool ShouldEscalateToFault(const ModuleState& module)
        {
            // Disabled/Suspended are passive-owned runtime states. Diagnostics may
            // update counters and messages, but they must not convert a deliberately
            // inactive module into Faulted and trigger a false Recovery Mode cycle.
            return !IsPassiveOwned(module);
        }

        static ModuleStatus DiagnosticErrorStatus(const ModuleState& module)
        {
            if (IsDisabledOwned(module)) return ModuleStatus::Disabled;
            if (IsSuspendedOwned(module)) return ModuleStatus::Suspended;
            return ModuleStatus::Faulted;
        }

        static bool IsExplicitFaultStillOwned(const ModuleState& module)
        {
            return module.explicitFault && module.status == ModuleStatus::Faulted && !IsPassiveOwned(module);
        }

    public:
        void Register(const std::string& id, const std::string& displayName, ModuleStatus status = ModuleStatus::Active, DWORD tick = GetTickCount())
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* existing = FindMutable(safeId);
            const std::string safeDisplayName = SanitizeCoreText(displayName.empty() ? safeId : displayName, 128);
            if (existing)
            {
                existing->displayName = safeDisplayName.empty() ? safeId : safeDisplayName;
                // Register() is a discovery/metadata path. Runtime-owned operating
                // state is controlled by SetStatus(), Report() and recovery helpers.
                // A repeated Register(..., Active) from initialization or late module
                // discovery must not rewrite an already Suspended/Disabled expected
                // state, otherwise a later recovery pass can revive an idle module.
                if (status == ModuleStatus::Disabled)
                {
                    existing->expectedStatus = ModuleStatus::Disabled;
                    existing->status = ModuleStatus::Disabled;
                    existing->explicitFault = false;
                }
                else if (status == ModuleStatus::Suspended)
                {
                    // Suspended discovery is weaker than a config-owned Disabled
                    // state. Preserve Disabled so a late registry refresh cannot
                    // half-wake a disabled module by changing only expectedStatus.
                    if (CanDiscoveryStatusReplaceExpected(*existing, ModuleStatus::Suspended))
                    {
                        existing->expectedStatus = ModuleStatus::Suspended;
                        if (existing->status != ModuleStatus::Faulted && existing->status != ModuleStatus::Disabled)
                            existing->status = ModuleStatus::Suspended;
                        if (existing->status != ModuleStatus::Faulted) existing->explicitFault = false;
                    }
                }
                else if (status == ModuleStatus::Faulted)
                {
                    existing->status = DiagnosticErrorStatus(*existing);
                    existing->explicitFault = existing->status == ModuleStatus::Faulted;
                }
                else
                {
                    // Active registration only confirms the module exists. Preserve
                    // explicit non-active expected states unless the module was
                    // already active. Use SetStatus(..., Active, ..., true) for
                    // intentional fault/suspend recovery.
                    if (existing->status == ModuleStatus::Active && existing->expectedStatus == ModuleStatus::Active)
                        existing->status = ModuleStatus::Active;
                }
                existing->lastHeartbeatTick = tick;
                return;
            }
            ModuleState module;
            module.id = safeId;
            module.displayName = safeDisplayName.empty() ? safeId : safeDisplayName;
            module.status = status;
            module.expectedStatus = status == ModuleStatus::Faulted ? ModuleStatus::Active : status;
            module.explicitFault = status == ModuleStatus::Faulted;
            module.lastHeartbeatTick = tick;
            if (modules_.size() >= kMaxModuleStates)
            {
                // Keep passive/faulted states as long as possible. They carry
                // config intent and diagnostic context; an active module is cheaper
                // to rediscover than a Disabled/Suspended/Faulted module is to lose.
                auto removable = std::find_if(modules_.begin(), modules_.end(), [](const ModuleState& state)
                {
                    return state.status == ModuleStatus::Active && state.expectedStatus == ModuleStatus::Active && !state.explicitFault;
                });
                if (removable == modules_.end())
                {
                    // Prefer non-passive, non-faulted entries first. Suspended and
                    // Disabled carry runtime/config ownership; evicting them before
                    // plain active aliases lets a later discovery pass recreate them
                    // as Active and undo the passive-state protections.
                    removable = std::find_if(modules_.begin(), modules_.end(), [](const ModuleState& state)
                    {
                        return state.status != ModuleStatus::Faulted &&
                               state.status != ModuleStatus::Disabled && state.expectedStatus != ModuleStatus::Disabled &&
                               state.status != ModuleStatus::Suspended && state.expectedStatus != ModuleStatus::Suspended &&
                               !state.explicitFault;
                    });
                }
                if (removable == modules_.end())
                {
                    // If the registry is truly full, drop a suspended entry before a
                    // disabled or explicit faulted entry. It can be rediscovered more
                    // safely than a disabled config-owned module or active fault context.
                    removable = std::find_if(modules_.begin(), modules_.end(), [](const ModuleState& state)
                    {
                        return state.status == ModuleStatus::Suspended || state.expectedStatus == ModuleStatus::Suspended;
                    });
                }
                if (removable == modules_.end())
                {
                    removable = std::find_if(modules_.begin(), modules_.end(), [](const ModuleState& state)
                    {
                        return state.status != ModuleStatus::Faulted && !state.explicitFault;
                    });
                }
                if (removable != modules_.end()) modules_.erase(removable);
                else modules_.erase(modules_.begin());
            }
            modules_.push_back(module);
        }

        void SetStatus(const std::string& id, ModuleStatus status, const std::string& message = std::string(), DWORD tick = GetTickCount(), bool allowFaultRecovery = false)
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* module = FindMutable(safeId);
            if (!module)
            {
                Register(safeId, safeId, status, tick);
                module = FindMutable(safeId);
            }
            if (!module) return;
            // Faulted is sticky for explicit runtime SetStatus/Register calls, but periodic health
            // checks such as Compatibility/Runtime maintenance must be able to clear
            // transient faults after the next successful scan. Without this, the UI
            // can show a module as faulted forever even after the underlying problem
            // has been fixed. Track the desired non-fault state separately so clearing
            // a transient fault does not briefly turn idle modules into Active.
            const bool preservePassiveRefresh = ShouldPreservePassiveExpectedOnStatusRefresh(*module, status, allowFaultRecovery);
            if (status != ModuleStatus::Faulted && !preservePassiveRefresh)
            {
                module->expectedStatus = status;
                module->explicitFault = false;
            }
            const ModuleStatus resolvedStatus = preservePassiveRefresh
                ? ResolveRecoveredStatus(*module, module->expectedStatus)
                : ((status == ModuleStatus::Faulted)
                    ? DiagnosticErrorStatus(*module)
                    : status);
            if (status == ModuleStatus::Faulted)
                module->explicitFault = resolvedStatus == ModuleStatus::Faulted;
            if (module->status != ModuleStatus::Faulted ||
                resolvedStatus == ModuleStatus::Faulted ||
                resolvedStatus == ModuleStatus::Disabled ||
                resolvedStatus == ModuleStatus::Suspended ||
                allowFaultRecovery ||
                preservePassiveRefresh)
                module->status = resolvedStatus;
            module->lastHeartbeatTick = tick;
            if (!message.empty()) module->lastMessage = SanitizeCoreText(message, 512);
        }

        void Heartbeat(const std::string& id, DWORD tick = GetTickCount())
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* module = FindMutable(safeId);
            if (!module) return;
            module->lastHeartbeatTick = tick;
        }

        void TouchDiagnostic(const std::string& id, ErrorSeverity severity, const std::string& message, DWORD tick = GetTickCount())
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* module = FindMutable(safeId);
            if (!module)
            {
                Register(safeId, safeId, ModuleStatus::Active, tick);
                module = FindMutable(safeId);
            }
            if (!module) return;
            module->lastHeartbeatTick = tick;
            const std::string safeMessage = SanitizeCoreText(message, 512);
            if (!safeMessage.empty()) module->lastMessage = safeMessage;

            // Used for ErrorReporter coalesced repeats. The diagnostic is still
            // current, but counters must not be incremented again. Keep the live
            // status in sync with severity while preserving Disabled/Suspended
            // ownership exactly like Report()/ReconcileFromEvents().
            if (severity == ErrorSeverity::Error)
            {
                module->status = DiagnosticErrorStatus(*module);
                if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
            }
            else if (severity == ErrorSeverity::Warning && module->status != ModuleStatus::Faulted && module->status != ModuleStatus::Disabled)
            {
                module->status = ResolveRecoveredStatus(*module, module->expectedStatus);
                if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
            }
        }

        void Report(const std::string& id, ErrorSeverity severity, const std::string& message, DWORD tick = GetTickCount())
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* module = FindMutable(safeId);
            if (!module)
            {
                Register(safeId, safeId, ModuleStatus::Active, tick);
                module = FindMutable(safeId);
            }
            if (!module) return;
            module->lastHeartbeatTick = tick;
            module->lastMessage = SanitizeCoreText(message, 512);
            if (severity == ErrorSeverity::Error)
            {
                SaturatingIncrement(module->errorCount);
                // Passive-owned modules are intentionally Disabled/Suspended. Runtime
                // errors may annotate counters/message, but must not promote those
                // modules to Faulted and start a false Recovery Mode loop.
                module->status = DiagnosticErrorStatus(*module);
                if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
            }
            else if (severity == ErrorSeverity::Warning)
            {
                SaturatingIncrement(module->warningCount);
                // Warnings annotate the current module, but they should not revive an
                // idle/suspended module as Active. Resolve through the same passive
                // ownership helper used by recovery so stale expectedStatus=Active from
                // older configs cannot wake a currently Suspended module.
                if (module->status != ModuleStatus::Faulted && module->status != ModuleStatus::Disabled)
                {
                    module->status = ResolveRecoveredStatus(*module, module->expectedStatus);
                    if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
                }
            }
        }

        void ClearTransientProblems(const std::string& id, ModuleStatus recoveredStatus, const std::string& message = std::string(), DWORD tick = GetTickCount())
        {
            const std::string safeId = SanitizeCoreText(id, 96);
            if (safeId.empty()) return;
            ModuleState* module = FindMutable(safeId);
            if (!module) return;
            module->errorCount = 0;
            module->warningCount = 0;
            // Clearing transient events must not resurrect disabled/suspended modules
            // or overwrite their desired passive state. Bulk recovery clears are often
            // issued with Active as a generic success state; for passive modules that
            // success state is only a message, not permission to become active later.
            const bool passiveOwned = IsPassiveOwned(*module);
            if (recoveredStatus != ModuleStatus::Faulted)
            {
                module->explicitFault = false;
                if (recoveredStatus == ModuleStatus::Disabled || recoveredStatus == ModuleStatus::Suspended || !passiveOwned)
                    module->expectedStatus = recoveredStatus;
            }
            else
            {
                module->explicitFault = DiagnosticErrorStatus(*module) == ModuleStatus::Faulted;
            }
            module->status = ResolveRecoveredStatus(*module, recoveredStatus);
            if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
            module->lastHeartbeatTick = tick;
            if (!message.empty()) module->lastMessage = SanitizeCoreText(message, 512);
        }

        void ReconcileFromEvents(const std::vector<ErrorEvent>& events, DWORD tick = GetTickCount())
        {
            // ErrorReporter is the source of truth for current bounded diagnostics.
            // SafeNative wrappers report directly to ErrorReporter, so the module
            // registry must be resynced from the event buffer or modules can remain
            // Active while the Core2 counter panel shows real errors/warnings.
            for (ModuleState& module : modules_)
            {
                module.errorCount = 0;
                module.warningCount = 0;
            }

            for (const ErrorEvent& event : events)
            {
                std::string safeId = SanitizeCoreText(event.module.empty() ? "core" : event.module, 96);
                if (safeId.empty()) safeId = "core";
                ModuleState* module = FindMutable(safeId);
                if (!module)
                {
                    Register(safeId, safeId, ModuleStatus::Active, tick);
                    module = FindMutable(safeId);
                }
                if (!module) continue;

                module->lastHeartbeatTick = event.tick == 0 ? tick : event.tick;
                module->lastMessage = SanitizeCoreText(event.message.empty() ? "No details" : event.message, 512);
                if (event.severity == ErrorSeverity::Error)
                {
                    SaturatingIncrement(module->errorCount);
                    module->status = DiagnosticErrorStatus(*module);
                    if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
                }
                else if (event.severity == ErrorSeverity::Warning)
                {
                    SaturatingIncrement(module->warningCount);
                    if (module->status != ModuleStatus::Faulted && module->status != ModuleStatus::Disabled)
                    {
                        module->status = ResolveRecoveredStatus(*module, module->expectedStatus);
                        if (module->status != ModuleStatus::Faulted) module->explicitFault = false;
                    }
                }
            }

            // When the bounded ErrorReporter has been cleared or has evicted old
            // events, the registry must not keep stale runtime states. Faulted is
            // only valid while a current error event is visible; plain Active or
            // Suspended can also be stale after a config/runtime transition if this
            // method is called from a counters-only refresh before the next full
            // UpdateCore2ModuleStatuses() pass. Normalize every module with no
            // current error back to its expected non-fault operating state, while
            // ResolveRecoveredStatus() keeps Disabled/Suspended ownership intact.
            for (ModuleState& module : modules_)
            {
                if (module.errorCount == 0)
                {
                    if (IsExplicitFaultStillOwned(module))
                        continue;
                    const ModuleStatus previousStatus = module.status;
                    module.status = ResolveRecoveredStatus(module, module.expectedStatus);
                    if (module.status != ModuleStatus::Faulted) module.explicitFault = false;
                    if (module.status != previousStatus && (module.lastMessage.empty() || module.warningCount == 0))
                        module.lastMessage = "No current diagnostics";
                }
            }
        }

        const std::vector<ModuleState>& Modules() const { return modules_; }

        int CountByStatus(ModuleStatus status) const
        {
            int count = 0;
            for (const ModuleState& module : modules_) if (module.status == status) ++count;
            return count;
        }

        std::string Summary() const
        {
            std::ostringstream ss;
            ss << modules_.size() << " modules / active " << CountByStatus(ModuleStatus::Active)
               << " / faulted " << CountByStatus(ModuleStatus::Faulted)
               << " / suspended " << CountByStatus(ModuleStatus::Suspended)
               << " / disabled " << CountByStatus(ModuleStatus::Disabled);
            return ss.str();
        }
    };

    class RuntimeRegistry
    {
        std::vector<RuntimeListReport> reports_;
        int totalRemoved_ = 0;
        int totalAfter_ = 0;

        static int SafeAdd(int current, int delta)
        {
            if (delta <= 0) return current;
            const int maxInt = std::numeric_limits<int>::max();
            return current > maxInt - delta ? maxInt : current + delta;
        }

        void RecountTotals()
        {
            totalRemoved_ = 0;
            totalAfter_ = 0;
            for (const RuntimeListReport& report : reports_)
            {
                totalRemoved_ = SafeAdd(totalRemoved_, report.removed);
                totalAfter_ = SafeAdd(totalAfter_, report.after);
            }
        }

    public:
        void BeginScan()
        {
            reports_.clear();
            totalRemoved_ = 0;
            totalAfter_ = 0;
        }

        void Record(const std::string& name, EntityKind kind, int before, int after, int removedOverride = -1)
        {
            RuntimeListReport report;
            report.name = SanitizeCoreText(name.empty() ? "runtime.unknown" : name, 128);
            if (report.name.empty()) report.name = "runtime.unknown";
            report.kind = kind;
            report.before = before < 0 ? 0 : before;
            report.after = after < 0 ? 0 : after;
            const int sizeDeltaRemoved = report.before > report.after ? report.before - report.after : 0;
            report.removed = removedOverride >= 0 ? removedOverride : sizeDeltaRemoved;
            if (report.removed < 0) report.removed = 0;
            if (reports_.size() >= kMaxRuntimeReports)
                reports_.erase(reports_.begin());
            reports_.push_back(report);
            RecountTotals();
        }

        void RecordCleanup(const std::string& name, EntityKind kind, int removed)
        {
            Record(name, kind, removed < 0 ? 0 : removed, 0, removed < 0 ? 0 : removed);
        }

        const std::vector<RuntimeListReport>& Reports() const { return reports_; }
        int TotalRemoved() const { return totalRemoved_; }
        int TotalAfter() const { return totalAfter_; }

        std::string Summary() const
        {
            std::ostringstream ss;
            ss << "runtime entities " << totalAfter_ << ", removed " << totalRemoved_;
            return ss.str();
        }
    };

    class TickBudgetController
    {
        int sampleCount_ = 0;
        int averageMs_ = 0;
        int lastMs_ = 0;
        int overBudgetCount_ = 0;
        bool throttled_ = false;
        DWORD throttleUntil_ = 0;

        static bool TickReached(DWORD now, DWORD target)
        {
            if (target == 0 || target == 0xFFFFFFFFUL) return true;
            return static_cast<int32_t>(now - target) >= 0;
        }

        static DWORD MakeFutureTick(DWORD now, DWORD intervalMs)
        {
            if (intervalMs > kMaxSafeTickIntervalMs) intervalMs = kMaxSafeTickIntervalMs;
            if (intervalMs == 0) intervalMs = 1u;
            const DWORD rawTarget = now + intervalMs;
            auto usableFuture = [&](DWORD candidate)
            {
                if (candidate == 0 || candidate == 0xFFFFFFFFUL || candidate == now) return false;
                const DWORD delta = static_cast<DWORD>(candidate - now);
                return delta > 0u && delta <= kMaxSafeTickIntervalMs;
            };
            if (usableFuture(rawTarget)) return rawTarget;
            // Try to keep the original interval when it lands on a sentinel.
            // Near wrap, raw+1 may leave the safe signed-delta window, while
            // raw-1 may be valid; try both directions before falling back.
            for (DWORD offset = 1u; offset <= 8u; ++offset)
            {
                const DWORD forward = rawTarget + offset;
                if (usableFuture(forward)) return forward;
                const DWORD backward = rawTarget - offset;
                if (usableFuture(backward)) return backward;
            }
            for (DWORD offset = 1u; offset <= 8u; ++offset)
            {
                const DWORD fallback = now + offset;
                if (usableFuture(fallback)) return fallback;
            }
            return now + 1u;
        }

    public:
        void Reset()
        {
            sampleCount_ = 0;
            averageMs_ = 0;
            lastMs_ = 0;
            overBudgetCount_ = 0;
            throttled_ = false;
            throttleUntil_ = 0;
        }

        void Record(DWORD now, DWORD durationMs, int budgetMs, int throttleMs = 2500)
        {
            if (budgetMs < 1) budgetMs = 1;
            if (throttleMs < 250) throttleMs = 250;
            if (static_cast<DWORD>(throttleMs) > kMaxSafeTickIntervalMs)
                throttleMs = static_cast<int>(kMaxSafeTickIntervalMs);
            lastMs_ = durationMs > static_cast<DWORD>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(durationMs);
            if (sampleCount_ <= 0)
            {
                sampleCount_ = 1;
                averageMs_ = lastMs_;
            }
            else
            {
                if (sampleCount_ < 1000000) ++sampleCount_;
                const long long weighted = (static_cast<long long>(averageMs_) * 7LL) + static_cast<long long>(lastMs_);
                const long long nextAverage = weighted / 8LL;
                averageMs_ = nextAverage > static_cast<long long>(std::numeric_limits<int>::max())
                    ? std::numeric_limits<int>::max()
                    : static_cast<int>(nextAverage < 0 ? 0 : nextAverage);
            }

            if (lastMs_ > budgetMs)
            {
                SaturatingIncrement(overBudgetCount_);
                throttled_ = true;
                throttleUntil_ = MakeFutureTick(now, static_cast<DWORD>(throttleMs));
            }
            else if (throttled_)
            {
                if (TickReached(now, throttleUntil_)) throttled_ = false;
            }
            else
            {
                throttled_ = false;
            }
        }

        bool Throttled(DWORD now)
        {
            if (throttled_ && TickReached(now, throttleUntil_)) throttled_ = false;
            return throttled_;
        }

        int LastMs() const { return lastMs_; }
        int AverageMs() const { return averageMs_; }
        int OverBudgetCount() const { return overBudgetCount_; }

        std::string Summary(int budgetMs) const
        {
            std::ostringstream ss;
            ss << "tick " << lastMs_ << "ms / avg " << averageMs_ << "ms / budget " << budgetMs << "ms";
            if (throttled_) ss << " / throttle";
            if (overBudgetCount_ > 0) ss << " / over " << overBudgetCount_;
            return ss.str();
        }
    };

    class CrashGuardController
    {
        int recoveryCount_ = 0;
        DWORD lastRecoveryTick_ = 0;
        std::string lastAction_ = "None";

    public:
        void RecordRecovery(const std::string& action, DWORD now = GetTickCount())
        {
            SaturatingIncrement(recoveryCount_);
            lastRecoveryTick_ = now;
            lastAction_ = SanitizeCoreText(action.empty() ? "recovery" : action, 256);
            if (lastAction_.empty()) lastAction_ = "recovery";
        }

        void Clear()
        {
            recoveryCount_ = 0;
            lastRecoveryTick_ = 0;
            lastAction_ = "None";
        }

        bool ShouldRecover(int errors, int warnings, int errorThreshold, int warningThreshold) const
        {
            errors = std::max(0, errors);
            warnings = std::max(0, warnings);
            if (errorThreshold < 1) errorThreshold = 1;
            if (warningThreshold < 1) warningThreshold = 1;
            return errors >= errorThreshold || warnings >= warningThreshold;
        }

        int RecoveryCount() const { return recoveryCount_; }
        DWORD LastRecoveryTick() const { return lastRecoveryTick_; }
        const std::string& LastAction() const { return lastAction_; }

        std::string Summary(int errors, int warnings) const
        {
            std::ostringstream ss;
            ss << "errors " << errors << " / warnings " << warnings << " / recoveries " << recoveryCount_;
            if (lastAction_ != "None") ss << " / last " << lastAction_;
            return ss.str();
        }
    };

    template <typename T>
    inline T ClampValue(T value, T minValue, T maxValue)
    {
        if (maxValue < minValue) return minValue;
        return std::max(minValue, std::min(value, maxValue));
    }

    template <typename T>
    inline void ClampIndex(int& index, const std::vector<T>& values)
    {
        if (values.empty())
        {
            index = 0;
            return;
        }
        if (index < 0 || index >= static_cast<int>(values.size())) index = 0;
    }

    inline void ClampIndex(int& index, int count)
    {
        if (count <= 0)
        {
            index = 0;
            return;
        }
        if (index < 0 || index >= count) index = 0;
    }

    inline int NextIndex(int index, int count)
    {
        if (count <= 0) return 0;
        if (index < 0 || index >= count) index = 0;
        return (index + 1) % count;
    }

    inline int PreviousIndex(int index, int count)
    {
        if (count <= 0) return 0;
        if (index < 0 || index >= count) index = 0;
        return index <= 0 ? count - 1 : index - 1;
    }

    inline bool IsFinite(float value)
    {
        return std::isfinite(value) != 0;
    }

    inline bool IsFiniteVector(const Vector3& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    inline Vector3 MakeVector(float x, float y, float z)
    {
        Vector3 v{};
        v.x = x;
        v.y = y;
        v.z = z;
        return v;
    }

    // Keep core-level native guards aligned with GTA V runtime/map bounds.
    // The old 100000.0f envelope allowed far-warped handles to pass safe-native
    // checks even though trainer-level systems reject them as unusable.
    static constexpr float kDefaultWorldXYLimit = 12000.0f;
    static constexpr float kDefaultWorldMinZ = -1000.0f;
    static constexpr float kDefaultWorldMaxZ = 10000.0f;
    static constexpr int kVehicleDriverSeat = -1;
    static constexpr int kVehicleMaxSafeSeat = 15;
    static constexpr float kMaxSafeVelocityComponent = 1000.0f;
    static constexpr float kMaxSafeForceComponent = 100000.0f;
    static constexpr float kMaxSafeEntitySpeed = 1000.0f;

    inline bool IsSafeVehicleSeatIndex(int seat)
    {
        // GTA V seat indexes are small signed values: -1 for driver, 0+ for passengers.
        // Rejecting extreme or corrupt config-derived values prevents undefined native calls.
        return seat >= kVehicleDriverSeat && seat <= kVehicleMaxSafeSeat;
    }

    inline int ClampVehicleSeatIndex(int seat, int fallback = kVehicleDriverSeat)
    {
        return IsSafeVehicleSeatIndex(seat) ? seat : (IsSafeVehicleSeatIndex(fallback) ? fallback : kVehicleDriverSeat);
    }

    inline bool IsUsableWorldVector(const Vector3& value, float minZ = kDefaultWorldMinZ, float maxZ = kDefaultWorldMaxZ)
    {
        if (!IsFiniteVector(value)) return false;
        if (maxZ < minZ) std::swap(maxZ, minZ);
        if (std::fabs(value.x) > kDefaultWorldXYLimit || std::fabs(value.y) > kDefaultWorldXYLimit) return false;
        return value.z >= minZ && value.z <= maxZ;
    }

    inline float NormalizeHeading(float heading, float fallback = 0.0f)
    {
        if (!IsFinite(heading)) heading = fallback;
        if (!IsFinite(heading)) heading = 0.0f;
        heading = std::fmod(heading, 360.0f);
        if (heading < 0.0f) heading += 360.0f;
        if (heading >= 360.0f) heading -= 360.0f;
        return heading;
    }

    inline bool IsLikelyNativeHandle(int handle)
    {
        // ScriptHookV/GTA transient handles for entities, cameras and blips are
        // positive integers. Reject zero/negative stale sentinel values before
        // calling native existence probes; some builds tolerate them, others do not.
        return handle > 0;
    }

    inline bool IsLikelyEntityHandle(Entity entity)
    {
        // ScriptHookV entity handles are positive transient IDs. Rejecting zero and
        // negative values before calling natives prevents stale/sentinel integers
        // from reaching DOES_ENTITY_EXIST on builds that are less tolerant.
        return IsLikelyNativeHandle(static_cast<int>(entity));
    }

    inline bool Exists(Entity entity)
    {
        if (!IsLikelyEntityHandle(entity)) return false;
        try { return ENTITY::DOES_ENTITY_EXIST(entity); }
        catch (...) { return false; }
    }

    inline bool IsPed(Entity entity)
    {
        if (!Exists(entity)) return false;
        try { return ENTITY::IS_ENTITY_A_PED(entity); }
        catch (...) { return false; }
    }

    inline bool IsPlayerPed(Entity entity)
    {
        if (!IsPed(entity)) return false;
        try { return PED::IS_PED_A_PLAYER(static_cast<Ped>(entity)); }
        catch (...) { return false; }
    }

    inline bool IsRuntimeTrackableEntity(Entity entity)
    {
        return Exists(entity) && !IsPlayerPed(entity);
    }

    inline bool IsVehicle(Entity entity)
    {
        if (!Exists(entity)) return false;
        try { return ENTITY::IS_ENTITY_A_VEHICLE(entity); }
        catch (...) { return false; }
    }

    inline bool IsObject(Entity entity)
    {
        if (!Exists(entity)) return false;
        try { return !ENTITY::IS_ENTITY_A_PED(entity) && !ENTITY::IS_ENTITY_A_VEHICLE(entity); }
        catch (...) { return false; }
    }

    inline EntityKind DetectEntityKind(Entity entity)
    {
        if (!Exists(entity)) return EntityKind::Any;
        try
        {
            if (ENTITY::IS_ENTITY_A_PED(entity)) return EntityKind::Ped;
            if (ENTITY::IS_ENTITY_A_VEHICLE(entity)) return EntityKind::Vehicle;
            return EntityKind::Object;
        }
        catch (...)
        {
            return EntityKind::Any;
        }
    }

    inline bool MatchesKind(Entity entity, EntityKind kind)
    {
        if (kind == EntityKind::Any) return Exists(entity);
        if (kind == EntityKind::Ped) return IsPed(entity);
        if (kind == EntityKind::Vehicle) return IsVehicle(entity);
        if (kind == EntityKind::Object) return IsObject(entity);
        return false;
    }

    inline bool TryGetEntityCoords(Entity entity, Vector3& out, bool requireUsableWorldVector = false)
    {
        // Always clear the output first. Several higher-level validators reuse a
        // stack Vector3 across probes; leaving stale coordinates on an invalid
        // handle can make a later fallback path look like a real entity position.
        out = {};
        if (!Exists(entity)) return false;
        try
        {
            out = ENTITY::GET_ENTITY_COORDS(entity, true);
        }
        catch (...)
        {
            out = {};
            return false;
        }
        if (!IsFiniteVector(out) || (requireUsableWorldVector && !IsUsableWorldVector(out)))
        {
            out = {};
            return false;
        }
        return true;
    }

    inline Vector3 SafeGetEntityCoords(Entity entity, const Vector3& fallback, bool requireUsableWorldVector = false)
    {
        Vector3 out{};
        if (TryGetEntityCoords(entity, out, requireUsableWorldVector)) return out;
        return fallback;
    }

    inline Hash SafeGetEntityModel(Entity entity, Hash fallback = 0)
    {
        if (!Exists(entity)) return fallback;
        try
        {
            return static_cast<Hash>(ENTITY::GET_ENTITY_MODEL(entity));
        }
        catch (...)
        {
            return fallback;
        }
    }

    inline Hash SafeGetTypedEntityModel(Entity entity, EntityKind kind, Hash fallback = 0)
    {
        if (!MatchesKind(entity, kind)) return fallback;
        return SafeGetEntityModel(entity, fallback);
    }

    inline Hash SafeGetPedModel(Ped ped, Hash fallback = 0)
    {
        return SafeGetTypedEntityModel(static_cast<Entity>(ped), EntityKind::Ped, fallback);
    }

    inline Hash SafeGetVehicleModel(Vehicle vehicle, Hash fallback = 0)
    {
        return SafeGetTypedEntityModel(static_cast<Entity>(vehicle), EntityKind::Vehicle, fallback);
    }

    inline Hash SafeGetObjectModel(Object object, Hash fallback = 0)
    {
        return SafeGetTypedEntityModel(static_cast<Entity>(object), EntityKind::Object, fallback);
    }

    inline bool TryGetEntityHeading(Entity entity, float& out)
    {
        out = 0.0f;
        if (!Exists(entity)) return false;
        try
        {
            out = static_cast<float>(ENTITY::GET_ENTITY_HEADING(entity));
        }
        catch (...)
        {
            return false;
        }
        if (!IsFinite(out)) return false;
        out = NormalizeHeading(out);
        return true;
    }

    inline float SafeGetEntityHeading(Entity entity, float fallback = 0.0f)
    {
        float heading = NormalizeHeading(fallback);
        float out = heading;
        return TryGetEntityHeading(entity, out) ? out : heading;
    }

    inline bool IsPedDeadOrDying(Ped ped, bool checkMeleeDeathFlags = true, bool fallbackWhenInvalid = true)
    {
        if (!IsPed((Entity)ped)) return fallbackWhenInvalid;
        try { return PED::IS_PED_DEAD_OR_DYING(ped, checkMeleeDeathFlags); }
        catch (...) { return fallbackWhenInvalid; }
    }

    inline int SafeGetEntityHealth(Entity entity, int fallback = 0)
    {
        if (!Exists(entity)) return fallback;
        try
        {
            const int health = static_cast<int>(ENTITY::GET_ENTITY_HEALTH(entity));
            if (health < 0 || health > 10000) return fallback;
            return health;
        }
        catch (...) { return fallback; }
    }

    inline bool TryGetEntityVelocity(Entity entity, Vector3& out)
    {
        out = {};
        // Velocity reads are not recovery operations. Reject stale/far-warped
        // entities before calling the native so diagnostics do not sample physics
        // from a handle that the rest of the trainer would immediately purge.
        Vector3 pos{};
        if (!TryGetEntityCoords(entity, pos, true)) return false;
        try { out = ENTITY::GET_ENTITY_VELOCITY(entity); }
        catch (...) { out = {}; return false; }
        if (!IsFiniteVector(out) ||
            std::fabs(out.x) > kMaxSafeVelocityComponent ||
            std::fabs(out.y) > kMaxSafeVelocityComponent ||
            std::fabs(out.z) > kMaxSafeVelocityComponent)
        {
            out = {};
            return false;
        }
        return true;
    }

    inline Vector3 SafeGetEntityVelocity(Entity entity, const Vector3& fallback = Vector3{})
    {
        Vector3 out{};
        return TryGetEntityVelocity(entity, out) ? out : fallback;
    }

    inline float SafeGetEntitySpeed(Entity entity, float fallback = 0.0f)
    {
        Vector3 pos{};
        if (!TryGetEntityCoords(entity, pos, true)) return fallback;
        try
        {
            const float speed = static_cast<float>(ENTITY::GET_ENTITY_SPEED(entity));
            if (!IsFinite(speed) || speed < 0.0f || speed > kMaxSafeEntitySpeed) return fallback;
            return speed;
        }
        catch (...)
        {
            return fallback;
        }
    }

    inline bool IsPedInAnyVehicle(Ped ped, bool atGetIn = false)
    {
        Vector3 pedPos{};
        if (!IsPed((Entity)ped) || !TryGetEntityCoords((Entity)ped, pedPos, true)) return false;
        try { return PED::IS_PED_IN_ANY_VEHICLE(ped, atGetIn); }
        catch (...) { return false; }
    }

    inline Vehicle GetVehiclePedIsIn(Ped ped, bool lastVehicle = false)
    {
        Vector3 pedPos{};
        if (!IsPed((Entity)ped) || !TryGetEntityCoords((Entity)ped, pedPos, true)) return 0;
        try
        {
            Vehicle vehicle = static_cast<Vehicle>(PED::GET_VEHICLE_PED_IS_IN(ped, lastVehicle));
            Vector3 vehiclePos{};
            return IsVehicle((Entity)vehicle) && TryGetEntityCoords((Entity)vehicle, vehiclePos, true) ? vehicle : 0;
        }
        catch (...)
        {
            return 0;
        }
    }

    inline bool IsPedInVehicle(Ped ped, Vehicle vehicle, bool atGetIn = false)
    {
        Vector3 pedPos{};
        Vector3 vehiclePos{};
        if (!IsPed((Entity)ped) || !TryGetEntityCoords((Entity)ped, pedPos, true) ||
            !IsVehicle((Entity)vehicle) || !TryGetEntityCoords((Entity)vehicle, vehiclePos, true)) return false;
        try { return PED::IS_PED_IN_VEHICLE(ped, vehicle, atGetIn); }
        catch (...) { return false; }
    }

    inline bool IsPedInCombat(Ped ped, Ped target = 0)
    {
        Vector3 pedPos{};
        if (!IsPed((Entity)ped) || !TryGetEntityCoords((Entity)ped, pedPos, true)) return false;
        if (target != 0)
        {
            Vector3 targetPos{};
            if (!IsPed((Entity)target) || !TryGetEntityCoords((Entity)target, targetPos, true)) return false;
        }
        try { return PED::IS_PED_IN_COMBAT(ped, target); }
        catch (...) { return false; }
    }

    inline bool IsPedShooting(Ped ped)
    {
        Vector3 pedPos{};
        if (!IsPed((Entity)ped) || !TryGetEntityCoords((Entity)ped, pedPos, true)) return false;
        try { return PED::IS_PED_SHOOTING(ped); }
        catch (...) { return false; }
    }

    inline bool HasUsableEntityCoords(Entity entity)
    {
        Vector3 pos{};
        return TryGetEntityCoords(entity, pos, true);
    }

    inline bool IsUsableTaskPed(Ped ped)
    {
        if (!IsPed((Entity)ped)) return false;
        if (IsPedDeadOrDying(ped, true, true)) return false;
        return HasUsableEntityCoords((Entity)ped);
    }

    inline Ped GetPedInVehicleSeat(Vehicle vehicle, int seat, Ped fallback = 0)
    {
        if (!IsSafeVehicleSeatIndex(seat)) return fallback;
        if (!IsVehicle((Entity)vehicle) || !HasUsableEntityCoords((Entity)vehicle)) return fallback;
        try
        {
            Ped ped = static_cast<Ped>(VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, seat));
            return IsPed((Entity)ped) ? ped : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    inline bool IsVehicleSeatFree(Vehicle vehicle, int seat, bool fallback = false)
    {
        if (!IsSafeVehicleSeatIndex(seat)) return fallback;
        if (!IsVehicle((Entity)vehicle) || !HasUsableEntityCoords((Entity)vehicle)) return fallback;
        try { return VEHICLE::IS_VEHICLE_SEAT_FREE(vehicle, seat); }
        catch (...) { return fallback; }
    }

    inline bool SetPedIntoVehicleSafe(Ped ped, Vehicle vehicle, int seat)
    {
        if (!IsSafeVehicleSeatIndex(seat)) return false;
        if (!IsUsableTaskPed(ped) || !IsVehicle((Entity)vehicle)) return false;
        if (!HasUsableEntityCoords((Entity)vehicle)) return false;
        const Ped occupant = GetPedInVehicleSeat(vehicle, seat, 0);
        if (occupant == ped) return true;
        if (occupant != 0) return false;
        try
        {
            PED::SET_PED_INTO_VEHICLE(ped, vehicle, seat);
            return IsPedInVehicle(ped, vehicle, false) || GetPedInVehicleSeat(vehicle, seat, 0) == ped;
        }
        catch (...)
        {
            return false;
        }
    }

    static constexpr DWORD kUnsetTick = 0xFFFFFFFFUL;

    inline DWORD ClampTickInterval(DWORD intervalMs)
    {
        // GetTickCount wraps every ~49.7 days. The standard unsigned-delta
        // comparison remains valid only while intervals stay below 2^31 ms.
        // Clamp corrupt/config-derived intervals so long-running sessions do not
        // stall maintenance gates after a wrap-around.
        return intervalMs > kMaxSafeTickIntervalMs ? kMaxSafeTickIntervalMs : intervalMs;
    }

    inline bool IsUnsetTickValue(DWORD tick)
    {
        return tick == 0 || tick == kUnsetTick;
    }

    inline bool TickReached(DWORD now, DWORD targetTick)
    {
        if (IsUnsetTickValue(targetTick)) return true;
        return static_cast<int32_t>(now - targetTick) >= 0;
    }

    inline DWORD TickDelta(DWORD now, DWORD lastTick)
    {
        if (IsUnsetTickValue(lastTick)) return 0;
        return static_cast<DWORD>(now - lastTick);
    }

    inline bool IsUsableFutureTick(DWORD now, DWORD candidate)
    {
        if (candidate == now || IsUnsetTickValue(candidate)) return false;
        const DWORD delta = static_cast<DWORD>(candidate - now);
        return delta > 0u && delta <= kMaxSafeTickIntervalMs;
    }

    inline DWORD TickAdd(DWORD now, DWORD intervalMs)
    {
        intervalMs = ClampTickInterval(intervalMs);
        if (intervalMs == 0) intervalMs = 1u;
        const DWORD rawTarget = now + intervalMs;
        if (IsUsableFutureTick(now, rawTarget)) return rawTarget;
        // Avoid the trainer's sentinel ticks even at the exact wrap boundary
        // without collapsing a long cooldown to one millisecond.
        for (DWORD offset = 1u; offset <= 8u; ++offset)
        {
            const DWORD forward = rawTarget + offset;
            if (IsUsableFutureTick(now, forward)) return forward;
            const DWORD backward = rawTarget - offset;
            if (IsUsableFutureTick(now, backward)) return backward;
        }
        for (DWORD offset = 1u; offset <= 8u; ++offset)
        {
            const DWORD fallback = now + offset;
            if (IsUsableFutureTick(now, fallback)) return fallback;
        }
        return now + 1u;
    }

    inline DWORD TickAdvance(DWORD baseTick, DWORD deltaMs)
    {
        if (IsUnsetTickValue(baseTick) || deltaMs == 0u) return baseTick;
        return TickAdd(baseTick, deltaMs);
    }

    inline bool TickElapsed(DWORD now, DWORD lastTick, DWORD intervalMs)
    {
        // Most runtime state fields use 0 as their default/unset tick, while
        // newer core-owned timers use kUnsetTick. Treat both as immediately
        // due so reset/init paths do not wait a full interval before the first
        // maintenance pass. Signed delta keeps wrap-around valid for clamped
        // intervals while preventing corrupt/future lastTick values from firing
        // every frame as a huge unsigned age.
        if (IsUnsetTickValue(lastTick)) return true;
        intervalMs = ClampTickInterval(intervalMs);
        if (intervalMs == 0) return true;
        const int32_t signedAge = static_cast<int32_t>(now - lastTick);
        if (signedAge < 0) return false;
        return static_cast<DWORD>(signedAge) >= intervalMs;
    }

    inline bool Due(DWORD now, DWORD& lastTick, DWORD intervalMs)
    {
        if (IsUnsetTickValue(lastTick) || TickElapsed(now, lastTick, intervalMs))
        {
            lastTick = now;
            return true;
        }
        return false;
    }

    template <typename Fn>
    inline ActionResult RunAction(const char* name, Fn&& fn)
    {
        try
        {
            std::forward<Fn>(fn)();
            return {};
        }
        catch (const std::exception& ex)
        {
            ActionResult result;
            result.ok = false;
            result.error = std::string(name ? name : "action") + " failed: " + ex.what();
            return result;
        }
        catch (...)
        {
            ActionResult result;
            result.ok = false;
            result.error = std::string(name ? name : "action") + " failed: unknown exception";
            return result;
        }
    }

    template <typename Fn>
    inline bool TryNativeVoid(Fn&& fn)
    {
        try
        {
            std::forward<Fn>(fn)();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    template <typename T, typename Fn>
    inline bool TryNativeValueChecked(T& out, T fallback, Fn&& fn)
    {
        try
        {
            out = static_cast<T>(std::forward<Fn>(fn)());
            return true;
        }
        catch (...)
        {
            out = fallback;
            return false;
        }
    }

    template <typename T, typename Fn>
    inline T TryNativeValue(T fallback, Fn&& fn)
    {
        T out = fallback;
        TryNativeValueChecked<T>(out, fallback, std::forward<Fn>(fn));
        return out;
    }

    template <typename T>
    inline int PurgeInvalidEntities(std::vector<T>& entities, bool requireUsableWorldVector = true, bool removeDuplicates = true)
    {
        int removed = 0;
        std::vector<T> kept;
        kept.reserve(entities.size());

        for (T value : entities)
        {
            Entity entity = static_cast<Entity>(value);
            Vector3 pos{};
            const bool invalid = !Exists(entity) || !TryGetEntityCoords(entity, pos, requireUsableWorldVector);
            const bool duplicate = !invalid && removeDuplicates && std::find(kept.begin(), kept.end(), value) != kept.end();
            if (invalid || duplicate)
            {
                ++removed;
                continue;
            }
            kept.push_back(value);
        }

        entities.swap(kept);
        return removed;
    }

    template <typename T>
    inline int PurgeInvalidEntitiesByKind(std::vector<T>& entities, EntityKind kind, bool requireUsableWorldVector = true, bool removeDuplicates = true)
    {
        int removed = 0;
        std::vector<T> kept;
        kept.reserve(entities.size());

        for (T value : entities)
        {
            Entity entity = static_cast<Entity>(value);
            Vector3 pos{};
            const bool invalid = !MatchesKind(entity, kind) || !TryGetEntityCoords(entity, pos, requireUsableWorldVector);
            const bool duplicate = !invalid && removeDuplicates && std::find(kept.begin(), kept.end(), value) != kept.end();
            if (invalid || duplicate)
            {
                ++removed;
                continue;
            }
            kept.push_back(value);
        }

        entities.swap(kept);
        return removed;
    }

    inline Vector3 ClampVectorComponents(Vector3 value, float maxAbs)
    {
        if (!IsFinite(maxAbs) || maxAbs < 0.0f) maxAbs = 0.0f;
        if (!IsFinite(value.x)) value.x = 0.0f;
        if (!IsFinite(value.y)) value.y = 0.0f;
        if (!IsFinite(value.z)) value.z = 0.0f;
        value.x = ClampValue(value.x, -maxAbs, maxAbs);
        value.y = ClampValue(value.y, -maxAbs, maxAbs);
        value.z = ClampValue(value.z, -maxAbs, maxAbs);
        return value;
    }

    namespace safe_native
    {
        inline bool ReportGuard(ErrorReporter* reporter, ErrorSeverity severity, const std::string& module, const std::string& action, const std::string& message)
        {
            if (reporter) reporter->Report(severity, module, action, message);
            return false;
        }

        inline bool RequireEntity(Entity entity, EntityKind kind, ErrorReporter* reporter, const std::string& module, const std::string& action)
        {
            if (!MatchesKind(entity, kind))
            {
                std::string message = std::string("Expected ") + EntityKindName(kind) + " handle, got invalid or different entity type";
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, message);
            }
            Vector3 pos{};
            if (!TryGetEntityCoords(entity, pos, true))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Entity has invalid world coordinates");
            return true;
        }

        inline bool RequireEntityHandle(Entity entity, EntityKind kind, ErrorReporter* reporter, const std::string& module, const std::string& action)
        {
            if (!MatchesKind(entity, kind))
            {
                std::string message = std::string("Expected ") + EntityKindName(kind) + " handle, got invalid or different entity type";
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, message);
            }
            return true;
        }

        inline bool RequireLiveTaskPed(Ped ped, ErrorReporter* reporter, const std::string& module, const std::string& action)
        {
            if (!RequireEntity((Entity)ped, EntityKind::Ped, reporter, module, action)) return false;
            if (IsPedDeadOrDying(ped, true, true))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Ped is dead or dying");
            return true;
        }

        inline bool RequireVehicleSeatAvailableForPed(Vehicle vehicle, int seat, Ped ped, ErrorReporter* reporter, const std::string& module, const std::string& action)
        {
            if (!IsSafeVehicleSeatIndex(seat))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Seat index is outside the safe GTA range");
            if (!RequireEntity((Entity)vehicle, EntityKind::Vehicle, reporter, module, action)) return false;
            const Ped occupant = GetPedInVehicleSeat(vehicle, seat, 0);
            if (occupant == ped) return true;
            if (occupant != 0)
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Vehicle seat is occupied by another ped");
            return true;
        }

        inline bool Freeze(Entity entity, bool frozen, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            // Freeze/unfreeze is frequently used by emergency recovery after an
            // entity already has bad world coordinates. Requiring a valid current
            // position here can block the exact recovery path that should fix it.
            if (!RequireEntityHandle(entity, EntityKind::Any, reporter, module, "freeze entity")) return false;
            if (!TryNativeVoid([&](){ ENTITY::FREEZE_ENTITY_POSITION(entity, frozen); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "freeze entity", "Native call failed safely");
            return true;
        }

        inline bool SetHeading(Entity entity, float heading, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set heading")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_HEADING(entity, NormalizeHeading(heading)); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set heading", "Native call failed safely");
            return true;
        }

        inline bool SetCoords(Entity entity, const Vector3& pos, bool noOffset = true, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!IsUsableWorldVector(pos)) return ReportGuard(reporter, ErrorSeverity::Warning, module, "set coords", "Target coordinates are invalid");
            // Do not require the entity's current coordinates to be sane here.
            // Teleport recovery often calls SetCoords specifically because the
            // existing position is below map, NaN, or otherwise unusable.
            if (!RequireEntityHandle(entity, EntityKind::Any, reporter, module, "set coords")) return false;
            const bool ok = TryNativeVoid([&]()
            {
                if (noOffset) ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, pos.x, pos.y, pos.z, false, false, false);
                else ENTITY::SET_ENTITY_COORDS(entity, pos.x, pos.y, pos.z, false, false, false, true);
            });
            if (!ok) return ReportGuard(reporter, ErrorSeverity::Warning, module, "set coords", "Native call failed safely");
            return true;
        }

        inline bool SetInvincible(Entity entity, bool enabled, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set invincible")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_INVINCIBLE(entity, enabled); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set invincible", "Native call failed safely");
            return true;
        }

        inline bool SetHealth(Entity entity, int health, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set health")) return false;
            const int safeHealth = health < 1 ? 1 : (health > 10000 ? 10000 : health);
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_HEALTH(entity, safeHealth); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set health", "Native call failed safely");
            return true;
        }

        inline bool SetMissionEntity(Entity entity, bool scriptHostObject = true, bool grabFromOtherScript = true, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntityHandle(entity, EntityKind::Any, reporter, module, "set mission entity")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_AS_MISSION_ENTITY(entity, scriptHostObject, grabFromOtherScript); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set mission entity", "Native call failed safely");
            return true;
        }

        inline bool SetVelocity(Entity entity, const Vector3& velocity, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!IsFiniteVector(velocity))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set velocity", "Velocity vector is invalid");
            const Vector3 safeVelocity = ClampVectorComponents(velocity, kMaxSafeVelocityComponent);
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set velocity")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_VELOCITY(entity, safeVelocity.x, safeVelocity.y, safeVelocity.z); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set velocity", "Native call failed safely");
            return true;
        }

        inline bool SetCollision(Entity entity, bool enabled, bool keepPhysics = true, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set collision")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_COLLISION(entity, enabled, keepPhysics); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set collision", "Native call failed safely");
            return true;
        }

        inline bool SetVisible(Entity entity, bool visible, bool unk = false, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set visible")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_VISIBLE(entity, visible, unk); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set visible", "Native call failed safely");
            return true;
        }

        inline bool ApplyForce(Entity entity, const Vector3& force, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!IsFiniteVector(force))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "apply force", "Force vector is invalid");
            const Vector3 safeForce = ClampVectorComponents(force, kMaxSafeForceComponent);
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "apply force")) return false;
            if (!TryNativeVoid([&](){ ENTITY::APPLY_FORCE_TO_ENTITY(entity, 1, safeForce.x, safeForce.y, safeForce.z, 0.0f, 0.0f, 0.0f, 0, false, true, true, false, true); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "apply force", "Native call failed safely");
            return true;
        }

        inline bool RepairVehicle(Vehicle vehicle, ErrorReporter* reporter = nullptr, const std::string& module = "vehicle")
        {
            if (!RequireEntity((Entity)vehicle, EntityKind::Vehicle, reporter, module, "repair vehicle")) return false;
            const bool ok = TryNativeVoid([&]()
            {
                VEHICLE::SET_VEHICLE_FIXED(vehicle);
                VEHICLE::SET_VEHICLE_ENGINE_HEALTH(vehicle, 1000.0f);
                VEHICLE::SET_VEHICLE_BODY_HEALTH(vehicle, 1000.0f);
                VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(vehicle);
                VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, true, false);
            });
            if (!ok) return ReportGuard(reporter, ErrorSeverity::Warning, module, "repair vehicle", "Native call failed safely");
            return true;
        }

        inline bool PutPedIntoVehicle(Ped ped, Vehicle vehicle, int seat, ErrorReporter* reporter = nullptr, const std::string& module = "vehicle")
        {
            constexpr const char* action = "put ped into vehicle";
            if (!RequireLiveTaskPed(ped, reporter, module, action)) return false;
            if (!RequireVehicleSeatAvailableForPed(vehicle, seat, ped, reporter, module, action)) return false;
            if (GetPedInVehicleSeat(vehicle, seat, 0) == ped) return true;
            const bool ok = TryNativeVoid([&](){ PED::SET_PED_INTO_VEHICLE(ped, vehicle, seat); });
            if (!ok) return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Native call failed safely");
            return IsPedInVehicle(ped, vehicle, false) || GetPedInVehicleSeat(vehicle, seat, 0) == ped;
        }

        inline bool TaskEnterVehicle(Ped ped, Vehicle vehicle, int timeoutMs, int seat, float speed, int flag, ErrorReporter* reporter = nullptr, const std::string& module = "vehicle")
        {
            constexpr const char* action = "task enter vehicle";
            if (!RequireLiveTaskPed(ped, reporter, module, action)) return false;
            if (!RequireVehicleSeatAvailableForPed(vehicle, seat, ped, reporter, module, action)) return false;
            if (GetPedInVehicleSeat(vehicle, seat, 0) == ped) return true;
            if (!IsFinite(speed)) speed = 1.0f;
            speed = ClampValue(speed, 0.1f, 50.0f);
            if (timeoutMs < -1) timeoutMs = -1;
            if (timeoutMs > 60000) timeoutMs = 60000;
            if (!TryNativeVoid([&](){ AI::TASK_ENTER_VEHICLE(ped, vehicle, timeoutMs, seat, speed, flag, 0); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Native call failed safely");
            return true;
        }

        inline bool TaskCombatPed(Ped ped, Ped target, ErrorReporter* reporter = nullptr, const std::string& module = "npc")
        {
            constexpr const char* action = "task combat ped";
            if (ped == target)
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Refused self-combat task");
            if (!RequireLiveTaskPed(ped, reporter, module, action)) return false;
            if (!RequireLiveTaskPed(target, reporter, module, action)) return false;
            if (!TryNativeVoid([&](){ AI::TASK_COMBAT_PED(ped, target, 0, 16); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, action, "Native call failed safely");
            return true;
        }

        inline bool SetProofs(Entity entity, bool bullet, bool fire, bool explosion, bool collision, bool melee, bool steam, bool p7, bool drown, ErrorReporter* reporter = nullptr, const std::string& module = "core")
        {
            if (!RequireEntity(entity, EntityKind::Any, reporter, module, "set proofs")) return false;
            if (!TryNativeVoid([&](){ ENTITY::SET_ENTITY_PROOFS(entity, bullet, fire, explosion, collision, melee, steam, p7, drown); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set proofs", "Native call failed safely");
            return true;
        }

        inline bool SetPedArmour(Ped ped, int armour, ErrorReporter* reporter = nullptr, const std::string& module = "ped")
        {
            if (!RequireLiveTaskPed(ped, reporter, module, "set ped armour")) return false;
            const int safeArmour = armour < 0 ? 0 : (armour > 500 ? 500 : armour);
            if (!TryNativeVoid([&](){ PED::SET_PED_ARMOUR(ped, safeArmour); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set ped armour", "Native call failed safely");
            return true;
        }

        inline bool SetPedCanRagdoll(Ped ped, bool canRagdoll, ErrorReporter* reporter = nullptr, const std::string& module = "ped")
        {
            if (!RequireLiveTaskPed(ped, reporter, module, "set ped ragdoll")) return false;
            if (!TryNativeVoid([&](){ PED::SET_PED_CAN_RAGDOLL(ped, canRagdoll); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set ped ragdoll", "Native call failed safely");
            return true;
        }

        inline bool ClearPedTasks(Ped ped, bool immediately = false, ErrorReporter* reporter = nullptr, const std::string& module = "ped")
        {
            if (!RequireLiveTaskPed(ped, reporter, module, immediately ? "clear ped tasks immediately" : "clear ped tasks")) return false;
            if (!TryNativeVoid([&](){ if (immediately) AI::CLEAR_PED_TASKS_IMMEDIATELY(ped); else AI::CLEAR_PED_TASKS(ped); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, immediately ? "clear ped tasks immediately" : "clear ped tasks", "Native call failed safely");
            return true;
        }

        inline bool SetPedToRagdoll(Ped ped, int minTimeMs, int maxTimeMs, int ragdollType = 0, bool p4 = true, bool p5 = true, bool p6 = false, ErrorReporter* reporter = nullptr, const std::string& module = "ped")
        {
            if (!RequireLiveTaskPed(ped, reporter, module, "set ped ragdoll state")) return false;
            minTimeMs = ClampValue(minTimeMs, 0, 60000);
            maxTimeMs = ClampValue(maxTimeMs, minTimeMs, 60000);
            ragdollType = ClampValue(ragdollType, 0, 3);
            if (!TryNativeVoid([&](){ PED::SET_PED_TO_RAGDOLL(ped, minTimeMs, maxTimeMs, ragdollType, p4, p5, p6); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "set ped ragdoll state", "Native call failed safely");
            return true;
        }

        inline bool DeletePedSafe(Ped& ped, ErrorReporter* reporter = nullptr, const std::string& module = "cleanup")
        {
            if (!ped || !Exists((Entity)ped))
            {
                ped = 0;
                return true;
            }
            if (!IsPed((Entity)ped))
            {
                ped = 0;
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete ped", "Tracked ped handle is not a ped anymore");
            }
            if (IsPlayerPed((Entity)ped))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete ped", "Refused to delete player ped");
            TryNativeVoid([&](){ ENTITY::SET_ENTITY_AS_MISSION_ENTITY((Entity)ped, true, true); });
            if (!TryNativeVoid([&](){ PED::DELETE_PED(&ped); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete ped", "Native call failed safely");
            if (!ped || !Exists((Entity)ped))
            {
                ped = 0;
                return true;
            }
            return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete ped", "Delete native returned but ped still exists");
        }

        inline bool DeleteVehicleSafe(Vehicle& vehicle, ErrorReporter* reporter = nullptr, const std::string& module = "cleanup")
        {
            if (!vehicle || !Exists((Entity)vehicle))
            {
                vehicle = 0;
                return true;
            }
            if (!IsVehicle((Entity)vehicle))
            {
                vehicle = 0;
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete vehicle", "Tracked vehicle handle is not a vehicle anymore");
            }
            TryNativeVoid([&](){ ENTITY::SET_ENTITY_AS_MISSION_ENTITY((Entity)vehicle, true, true); });
            if (!TryNativeVoid([&](){ VEHICLE::DELETE_VEHICLE(&vehicle); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete vehicle", "Native call failed safely");
            if (!vehicle || !Exists((Entity)vehicle))
            {
                vehicle = 0;
                return true;
            }
            return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete vehicle", "Delete native returned but vehicle still exists");
        }

        inline bool DeleteObjectSafe(Object& object, ErrorReporter* reporter = nullptr, const std::string& module = "cleanup")
        {
            if (!object || !Exists((Entity)object))
            {
                object = 0;
                return true;
            }
            if (!IsObject((Entity)object))
            {
                object = 0;
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete object", "Tracked object handle is not an object anymore");
            }
            TryNativeVoid([&](){ ENTITY::SET_ENTITY_AS_MISSION_ENTITY((Entity)object, true, true); });
            if (!TryNativeVoid([&](){ OBJECT::DELETE_OBJECT(&object); }))
                return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete object", "Native call failed safely");
            if (!object || !Exists((Entity)object))
            {
                object = 0;
                return true;
            }
            return ReportGuard(reporter, ErrorSeverity::Warning, module, "delete object", "Delete native returned but object still exists");
        }
    }
}
