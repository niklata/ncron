#ifndef NKLIB_SCOPEGUARD_HPP_
#define NKLIB_SCOPEGUARD_HPP_

#include <utility>

// Usage: nk::scope_guard NAME = [&]{ LAMBDA_CONTENTS };
// And the lambda will be invoked when NAME goes out of scope.
//
// If the lambda shouldn't be called when going out of scope, call: NAME.dismiss();

namespace nk {
    template <typename Fn>
    class scope_guard final {
        Fn f_;
        bool active_;
    public:
        constexpr scope_guard(Fn f) : f_(std::forward<Fn>(f)), active_(true) {}
        ~scope_guard() { if (active_) f_(); }

        scope_guard() = delete;
        scope_guard(const scope_guard&) = delete;
        scope_guard& operator=(const scope_guard&) = delete;

        constexpr void dismiss() { active_ = false; }
    };
}

// Usage: defer [&]{ LAMBDA_CONTENTS };
// And the lambda will be invoked when NAME goes out of scope.

#define NK_DEFER_CONCAT_HELPER(a, b) a##b
#define NK_DEFER_CONCAT(a,b) NK_DEFER_CONCAT_HELPER(a, b)
#define NK_DEFER_APPEND_COUNTER(x) NK_DEFER_CONCAT(x, __COUNTER__)

namespace nk::detail {
    template <typename Fn>
    class defer_guard final {
        Fn f_;
    public:
        constexpr defer_guard(Fn f) : f_(std::forward<Fn>(f)) {}
        ~defer_guard() { f_(); }

        defer_guard() = delete;
        defer_guard(const defer_guard&) = delete;
        defer_guard& operator=(const defer_guard&) = delete;
        defer_guard(defer_guard&&) = delete;
        defer_guard& operator=(defer_guard&&) = delete;
    };
    struct defer_guard_helper {
        template <typename Fn>
        defer_guard<Fn> operator+(Fn f) { return f; }
    };
}

#define defer [[maybe_unused]] const auto & NK_DEFER_APPEND_COUNTER(NK_DEFER_) = nk::detail::defer_guard_helper() +

#endif
