#pragma once

#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

class ExecutionContext;
class Notification;

enum class ThreadState;

class Baton : public KernelObject {
public:
    std::uint64_t budget{0};             // 残余CPU時間（マイクロ秒: us）
    bool is_infinite{false};             // 根源の無限バトン（Root Baton）フラグ
    ExecutionContext* thread{nullptr};   // 現在このバトンが紐づいているコンテキスト（0個または1個）
    ExecutionContext* caller_context{nullptr}; // 直前の switch 元スレッド (コルーチン呼出元)
    Notification* bound_notif{nullptr};  // 再開通知用 Notification
    std::uint64_t bound_badge{0};        // 再開通知用 Badge
    bool is_active{false};               // 現在ディスパッチチェーンに含まれているか
    bool is_irq_baton{false};            // 割り込みハンドラ専用バトンの印

    explicit Baton(bool untyped = false) : KernelObject(ObjectType::Baton, untyped) {}
    ~Baton() override;

    bool is_runnable() const;
    bool is_dispatchable() const {
        return is_runnable() && has_budget();
    }
    bool has_budget() const { return is_infinite || budget > 0; }

    void notify_if_runnable();
};

} // namespace oz
