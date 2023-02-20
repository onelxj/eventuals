#pragma once

#include "eventuals/expected.h"
#include "eventuals/terminal.h" // For 'Stopped'.
#include "eventuals/then.h"

////////////////////////////////////////////////////////////////////////

namespace eventuals {

////////////////////////////////////////////////////////////////////////

template <typename...>
struct StoppedOrVariantFrom {};

template <typename... Errors>
struct StoppedOrVariantFrom<std::tuple<Errors...>> {
  using type = std::exception_ptr;
  // fix task.h to not use std::exception_ptr
  // std::conditional_t<
  //     sizeof...(Errors) == 0,
  //     Stopped,
  //     std::variant<Stopped, Errors...>>;
};

////////////////////////////////////////////////////////////////////////

struct _Finally final {
  template <typename K_, typename Arg_, typename Errors_>
  struct Continuation final {
    template <typename... Args>
    void Start(Args&&... args) {
      k_.Start(
          expected<Arg_, Errors_>(
              std::forward<Args>(args)...));
    }

    template <typename Error>
    void Fail(Error&& error) {
      k_.Start(
          expected<Arg_, Errors_>(
              make_unexpected(
                  std::make_exception_ptr(std::forward<Error>(error)))));
    }

    void Stop() {
      k_.Start(
          expected<Arg_, Errors_>(
              make_unexpected(
                  std::make_exception_ptr(Stopped()))));
    }

    void Register(Interrupt& interrupt) {
      k_.Register(interrupt);
    }

    K_ k_;
  };

  struct Composable final {
    template <typename Arg, typename Errors>
    using ValueFrom =
        expected<Arg, typename StoppedOrVariantFrom<Errors>::type>;

    template <typename Arg, typename Errors>
    using ErrorsFrom = std::tuple<>;

    template <typename Arg, typename Errors, typename K>
    auto k(K k) && {
      return Continuation<
          K,
          Arg,
          typename StoppedOrVariantFrom<Errors>::type>{std::move(k)};
    }

    template <typename Downstream>
    static constexpr bool CanCompose = Downstream::ExpectsValue;

    using Expects = SingleValue;
  };
};

////////////////////////////////////////////////////////////////////////

template <typename F>
[[nodiscard]] auto Finally(F f) {
  return _Finally::Composable()
      >> Then(std::move(f));
}

////////////////////////////////////////////////////////////////////////

} // namespace eventuals

////////////////////////////////////////////////////////////////////////
