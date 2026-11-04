#pragma once

#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

class ExecutionContext;
class Notification;
class IRQHandler;

enum class ThreadState;

enum class BatonState : std::uint8_t {
    READY,        // アイドル状態。チェーン未所属
    RUNNING,      // 現在 CPU を実行中 (委譲チェーンの末尾ノード)
    WAIT_RETURN,  // 子へ委譲して待機中
    WAIT_START,   // 上位タイムアウトにより分断・一時停止中 (フォワーディング対象)
    BLOCKED       // IPCやI/O待機中
};

class Baton : public KernelObject {
public:
    // --- タイムスライス・仮想時間 ---
    std::uint64_t quantum{0};                    // 直近スイッチ時に明示指定されたクォンタム (us)
    union {
        std::uint64_t remaining{0};              // メモリ上の記録残り時間 (us)
        std::uint64_t budget;                    // 既存コード互換用エイリアス
    };
    std::uint64_t last_sync_tsc{0};              // 最終同期TSC
    bool is_infinite{false};                     // ルートバトン専用: 無限仮想時間フラグ

    // --- 委譲チェーン (Call Stack) ポインタ ---
    Baton* caller_baton{nullptr};                // 呼出元バトン (親方向)
    Baton* callee_baton{nullptr};                // 呼出先バトン (子方向)
    Baton* tail_baton{nullptr};                  // サブチェーンの真の実行末尾 ★フォワーディング用

    // --- デカルト木 (Cartesian Tree) ポインタ (計24バイト) ---
    Baton* cartesian_parent{nullptr};            // 木の親ノード
    Baton* cartesian_left{nullptr};              // 左の子 (自分より親側の最小ノード)
    Baton* cartesian_right{nullptr};             // 右の子 (自分より子側の最小ノード ★定理1)

    // --- 中断サブチェーン管理 & 遅延評価タグ ---
    Baton* sub_min_baton{nullptr};               // 配下サブチェーンの最短ノードキャッシュ
    std::uint64_t lazy_delta{0};                 // 部分木に未適用の遅延減算タグ (Push-Down用 ★O(1)分断)
    std::uint64_t subchain_consumed_offset{0};   // 分断中累積消費時間 (デバッグ・復帰検証用)

    // --- バインド情報 ---
    ExecutionContext* thread{nullptr};           // 紐づく実行コンテキスト
    BatonState state{BatonState::READY};         // バトン状態マシン

    // --- 既存システム互換用 ---
    ExecutionContext* caller_context{nullptr};   // 直前の switch 元スレッド (コルーチン呼出元)
    Notification* bound_notif{nullptr};          // 再開通知用 Notification
    std::uint64_t bound_badge{0};                // 再開通知用 Badge
    bool is_active{false};                       // 現在ディスパッチチェーンに含まれているか
    bool is_irq_baton{false};                    // 割り込みハンドラ専用バトンの印
    IRQHandler* bound_irq_handler{nullptr};      // 次回割り込み待機中の IRQHandler

    // Global registry intrusive list
    Baton* reg_prev_{nullptr};
    Baton* reg_next_{nullptr};

    explicit Baton(bool untyped = false);
    ~Baton() override;

    inline void resetCartesian() {
        cartesian_parent = nullptr;
        cartesian_left = nullptr;
        cartesian_right = nullptr;
        lazy_delta = 0;
    }

    inline std::uint64_t get_effective_remaining() const {
        if (is_infinite) return ~0ULL;
        return (remaining > lazy_delta) ? (remaining - lazy_delta) : 0;
    }

    bool is_runnable() const;
    bool is_dispatchable() const {
        return is_runnable() && has_budget();
    }
    bool has_budget() const {
        return is_infinite || get_effective_remaining() > 0;
    }

    void notify_if_runnable();
};

void unbind_notification_from_all_batons(Notification* notif);
void unbind_thread_from_all_batons(ExecutionContext* ctx);

} // namespace oz
