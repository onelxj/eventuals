#pragma once

#include "stout/continuation.h"
#include "stout/terminal.h"
#include "stout/undefined.h"

// TODO(benh): lifecycle management, i.e., don't let an eventual get
// started more than once or don't let an eventual get stopped if it
// hasn't been started. this is a little tricky because using
// something like a 'std::atomic' makes the Eventual neither copyable
// not moveable so we'd need some sort of wrapper around the Eventual
// (maybe an Operation, kind of like Task but without the
// std::promise/future). once we have lifecycle management then we can
// also make sure that after an eventual has been started we won't let
// it's destructor get executed until after it's terminated (i.e.,
// it's Terminal has been completed). Related: how to make sure that
// 'stopped()' is not called if 'succeeded()' is called? threre can be
// an outstanding call to 'stop()' while 'start()' is still executing,
// so is it up to the programmer to coordinate that they won't call
// both 'succeeded()' and 'stopped()'?
//
// TODO(benh): catch exceptions from 'start', 'fail', 'stop', etc.
//
// TODO(benh): create a 'then' which is a continuation that propagates
// 'fail' and 'stop' (note this is different then just using composing
// with a function because a 'then' would take a continuation 'k').
//
// TODO(benh): composing non-continuation eventual that doesn't have a
// 'fail' handler instead will need to propagate the failure past the
// eventual to the continuation.
//
// TODO(benh): eventual/continuation with no context should allow for
// functions that don't require a context either.
//
// TODO(benh): aggregate errors across all the eventuals.
//
// TODO(benh): lambda visitor for matching errors.

namespace stout {
namespace eventuals {

template <typename K>
void start(K& k)
{
  static_assert(
      HasTerminal<K>::value,
      "Trying to start a continuation that never terminates!");

  static_assert(
      !std::is_same_v<K, Undefined>,
      "You're starting a continuation that goes nowhere!");

  k.Start();
}


// TODO(benh): Overload with no 't'.
template <typename K, typename T>
void succeed(K& k, T&& t)
{
  static_assert(
      !std::is_same_v<K, Undefined>,
      "You're succedding a continuation that goes nowhere!");

  k.Succeed(std::forward<T>(t));
}


template <typename K, typename Error>
void fail(K& k, Error&& error)
{
  static_assert(
      !std::is_same_v<K, Undefined>,
      "You're failing a continuation that goes nowhere!");

  k.Fail(std::forward<Error>(error));
}


template <typename K>
void stop(K& k)
{
  static_assert(
      HasTerminal<K>::value,
      "Trying to stop a continuation that never terminates!");

    static_assert(
      !std::is_same_v<K, Undefined>,
      "You're stopping a continuation that goes nowhere!");

  k.Stop();
}


template <typename>
struct IsEventual : std::false_type {};


namespace detail {

template <
  typename Value_,
  typename K_,
  typename Context_,
  typename Start_,
  typename Fail_,
  typename Stop_,
  typename... Errors_>
struct Eventual
{
  using Value = Value_;

  K_ k_;

  Context_ context_;
  Start_ start_;
  Fail_ fail_;
  Stop_ stop_;

  Eventual(const Eventual& that) = default;
  Eventual(Eventual&& that) = default;

  Eventual& operator=(const Eventual& that) = default;
  Eventual& operator=(Eventual&& that)
  {
    // TODO(benh): Change this to use 'swap' or investigate why the
    // compiler needs us to define this in the first place and can't
    // just resolve the move assignment operators for all the fields.
    this->~Eventual();
    new(this) Eventual(std::move(that));
    return *this;
  }

  template <
    typename Value,
    typename... Errors,
    typename K,
    typename Context,
    typename Start,
    typename Fail,
    typename Stop>
  static auto create(
      K k,
      Context context,
      Start start,
      Fail fail,
      Stop stop)
  {
    return Eventual<
      Value,
      K,
      Context,
      Start,
      Fail,
      Stop,
      Errors...> {
      std::move(k),
      std::move(context),
      std::move(start),
      std::move(fail),
      std::move(stop)
    };
  }

  template <
    typename F,
    std::enable_if_t<
      !IsContinuation<F>::value, int> = 0>
  auto k(F f) &&
  {
    // Handle non-eventuals that are *invocable*.
    return std::move(*this).k(
        create<decltype(f(std::declval<Value>()))>(
            Undefined(),
            std::move(f),
            [](auto& f, auto& k, auto&& value) {
              stout::eventuals::succeed(k, f(std::forward<decltype(value)>(value)));
            },
            [](auto&, auto& k, auto&& error) {
              stout::eventuals::fail(k, std::forward<decltype(error)>(error));
            },
            [](auto&, auto& k) {
              stout::eventuals::stop(k);
            }));
  }

  template <
    typename K,
    std::enable_if_t<
      IsContinuation<K>::value, int> = 0>
  auto k(K k) &&
  {
    static_assert(
        !IsUndefined<K_>::value || IsEventual<K>::value || IsTerminal<K>::value,
        "Expecting an 'Eventual' or a 'Terminal'");

    static_assert(
        !IsTerminal<K>::value || !HasTerminal<K_>::value,
        "Redundant 'Terminal'");

    using Value = std::conditional_t<
      IsTerminal<K>::value,
      Value_,
      typename K::Value>;

    static_assert(
        !IsUndefined<Value>::value,
        "Continuation Value is Undefined?");

    return create<Value, Errors_...>(
        [&]() {
          if constexpr (!IsUndefined<K_>::value) {
            return std::move(k_) | std::move(k);
          } else {
            return std::move(k);
          }
        }(),
        std::move(context_),
        std::move(start_),
        std::move(fail_),
        std::move(stop_));
  }

  template <typename Context>
  auto context(Context context) &&
  {
    static_assert(IsUndefined<Context_>::value, "Duplicate 'context'");
    return create<Value_, Errors_...>(
        std::move(k_),
        std::move(context),
        std::move(start_),
        std::move(fail_),
        std::move(stop_));
  }

  template <typename Start>
  auto start(Start start) &&
  {
    static_assert(IsUndefined<Start_>::value, "Duplicate 'start'");
    return create<Value_, Errors_...>(
        std::move(k_),
        std::move(context_),
        std::move(start),
        std::move(fail_),
        std::move(stop_));
  }

  template <typename Fail>
  auto fail(Fail fail) &&
  {
    static_assert(IsUndefined<Fail_>::value, "Duplicate 'fail'");
    return create<Value_, Errors_...>(
        std::move(k_),
        std::move(context_),
        std::move(start_),
        std::move(fail),
        std::move(stop_));
  }

  template <typename Stop>
  auto stop(Stop stop) &&
  {
    static_assert(IsUndefined<Stop_>::value, "Duplicate 'stop'");
    return create<Value_, Errors_...>(
        std::move(k_),
        std::move(context_),
        std::move(start_),
        std::move(fail_),
        std::move(stop));
  }

  void Start()
  {
    static_assert(
        !IsUndefined<Start_>::value,
        "Undefined 'start' (and no default)");

    if constexpr (IsUndefined<Context_>::value) {
      start_(k_);
    } else {
      start_(context_, k_);
    }
  }

  template <typename T>
  void Succeed(T&& t)
  {
    static_assert(
        !IsUndefined<Start_>::value,
        "Undefined 'start' (and no default)");

    if constexpr (IsUndefined<Context_>::value) {
      start_(k_, std::forward<T>(t));
    } else {
      start_(context_, k_, std::forward<T>(t));
    }
  }

  template <typename Error>
  void Fail(Error&& error)
  {
    if constexpr (IsUndefined<Fail_>::value) {
      stout::eventuals::fail(k_, std::forward<Error>(error));
    } else if constexpr (IsUndefined<Context_>::value) {
      fail_(k_, std::forward<Error>(error));
    } else {
      fail_(context_, k_, std::forward<Error>(error));
    }
  }

  void Stop()
  {
    static_assert(
        !IsUndefined<Stop_>::value,
        "Undefined 'stop' (and no default)");

    if constexpr (IsUndefined<Context_>::value) {
      stop_(k_);
    } else {
      stop_(context_, k_);
    }
  }
};

} // namespace detail {


template <
  typename Value,
  typename K,
  typename Context,
  typename Start,
  typename Fail,
  typename Stop,
  typename... Errors>
struct IsEventual<
  detail::Eventual<
    Value,
    K,
    Context,
    Start,
    Fail,
    Stop,
    Errors...>> : std::true_type {};


template <
  typename Value,
  typename K,
  typename Context,
  typename Start,
  typename Fail,
  typename Stop,
  typename... Errors>
struct IsContinuation<
  detail::Eventual<
    Value,
    K,
    Context,
    Start,
    Fail,
    Stop,
    Errors...>> : std::true_type {};


template <
  typename Value,
  typename K,
  typename Context,
  typename Start,
  typename Fail,
  typename Stop,
  typename... Errors>
struct HasTerminal<
  detail::Eventual<
    Value,
    K,
    Context,
    Start,
    Fail,
    Stop,
    Errors...>> : HasTerminal<K> {};


template <typename Value, typename... Errors>
auto Eventual()
{
  return detail::Eventual<
    Value,
    Undefined,
    Undefined,
    Undefined,
    Undefined,
    Undefined,
    Errors...> {
    Undefined(),
    Undefined(),
    Undefined(),
    Undefined(),
    Undefined()
  };
}

} // namespace eventuals {
} // namespace stout {
