#pragma once

namespace oz {

// ユーザー空間向け軽量スピンロック (freestanding 組み込み関数利用)
class UserSpinlock {
private:
    char flag_{0};
public:
    void lock() {
        while (__atomic_test_and_set(&flag_, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }
    void unlock() {
        __atomic_clear(&flag_, __ATOMIC_RELEASE);
    }
};

class UserScopedLock {
private:
    UserSpinlock& lock_;
public:
    explicit UserScopedLock(UserSpinlock& l) : lock_(l) { lock_.lock(); }
    ~UserScopedLock() { lock_.unlock(); }
};

} // namespace oz
