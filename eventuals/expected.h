#pragma once

#include "eventuals/compose.h"
#include "eventuals/eventual.h"
#include "tl/expected.hpp"

////////////////////////////////////////////////////////////////////////

namespace eventuals {

////////////////////////////////////////////////////////////////////////

// Helper for creating an eventual from an 'expected'.
template <typename T, typename E>
auto ExpectedToEventual(tl::expected<T, E>&& expected) {
  // TODO(benh): support any error type that can be "stringified".
  static_assert(
      std::disjunction_v<
          check_errors_t<E>,
          std::is_same<std::string, std::decay_t<E>>,
          std::is_same<char*, std::decay_t<E>>>,
      "To use an 'expected' as an eventual it must have "
      "an error type derived from 'eventuals::Error', "
      "or be string-like");

  return Eventual<T>()
      .template raises<
          std::conditional_t<
              check_errors_v<E>,
              E,
              RuntimeError>>()
      // NOTE: we only care about "start" here because on "stop"
      // or "fail" we want to propagate that. We don't want to
      // override a "stop" with our failure because then
      // downstream eventuals might not stop but instead try and
      // recover from the error.
      .start([expected = std::move(expected)](auto& k) mutable {
        if (expected.has_value()) {
          if constexpr (std::is_void_v<T>) {
            return k.Start();
          } else {
            return k.Start(std::move(expected.value()));
          }
        } else {
          if constexpr (check_errors_v<E>) {
            return k.Fail(std::move(expected.error()));
          } else {
            return k.Fail(RuntimeError(std::move(expected.error())));
          }
        }
      });
}

// Helper for creating an eventual from an 'expected' with 'std::variant'
// errors.
template <typename T, typename... Errors>
auto ExpectedToEventual(
    tl::expected<T, std::variant<Stopped, Errors...>>&& expected) {
  // TODO(benh): support any error type that can be "stringified".
  static_assert(
      check_errors_v<Errors...>,
      "To use an 'expected' with 'std::variant' errors as an eventual "
      "it must have all error types derived from 'eventuals::Error' "
      "or be string-like");

  return Eventual<T>()
      .template raises<Errors...>()
      // NOTE: we only care about "start" here because on "stop"
      // or "fail" we want to propagate that. We don't want to
      // override a "stop" with our failure because then
      // downstream eventuals might not stop but instead try and
      // recover from the error.
      .start([expected = std::move(expected)](auto& k) mutable {
        if (expected.has_value()) {
          if constexpr (std::is_void_v<T>) {
            return k.Start();
          } else {
            return k.Start(std::move(expected.value()));
          }
        } else {
          if constexpr (check_errors_v<Errors...>) {
            std::visit(
                [&k](auto&& error) {
                  if constexpr (std::is_same_v<
                                    std::decay_t<decltype(error)>,
                                    Stopped>) {
                    return k.Stop();
                  } else {
                    return k.Fail(std::forward<decltype(error)>(error));
                  }
                },
                std::move(expected.error()));
          } else {
            return k.Fail(RuntimeError(std::move(expected.error())));
          }
        }
      });
}

template <typename T>
auto ExpectedToEventual(tl::expected<T, Stopped>&& expected) {
  return Eventual<T>()
      // NOTE: we only care about "start" here because on "stop"
      // or "fail" we want to propagate that. We don't want to
      // override a "stop" with our failure because then
      // downstream eventuals might not stop but instead try and
      // recover from the error.
      .start([expected = std::move(expected)](auto& k) mutable {
        if (expected.has_value()) {
          if constexpr (std::is_void_v<T>) {
            return k.Start();
          } else {
            return k.Start(std::move(expected.value()));
          }
        } else {
          return k.Stop();
        }
      });
}

////////////////////////////////////////////////////////////////////////

// Wrapper for 'tl::expected' that allows it to seamlessly be composed
// with other eventuals.
//
// It's currently not possible to not wrap 'tl::expected' without
// dramatic changes to how we compose eventuals (e.g., we'd likely
// need to make 'k()' be a free-standing function and 'ValueFrom' and
// 'ErrorsFrom' be free-standing types).
//
// Even if we did that, however, we may still find ourselves wanting
// to have an "eventuals flavored expected" that is syntactic sugar
// for a 'std::expected'. For example, 'eventuals::expected<T>' might
// be an alias for 'std::expected<T, eventuals::Status>' where
// 'eventuals::Status' is an eventuals specific error type. We might
// then also have 'eventuals::grpc::expected<T>' be an alias for
// 'std::expected<T, eventuals::grpc::Status>', and so forth and so
// on.
//
// For now, an 'eventuals::expected<T>' uses a 'std::string' as the
// default error type to simplify calls to 'make_unexpected("...")'
// that take strings which is the majority (if not all) of calls.
template <typename Value_, typename Error_ = std::string>
class expected : public tl::expected<Value_, Error_> {
 public:
  // Providing 'ValueFrom, 'ErrorsFrom', and 'k()' to be able to
  // compose with other eventuals.
  template <typename Arg, typename Errors>
  using ValueFrom = Value_;

  template <typename Arg, typename Errors>
  using ErrorsFrom = tuple_types_union_t<
      std::tuple<
          std::conditional_t<
              std::is_base_of_v<Error, std::decay_t<Error_>>,
              Error_,
              RuntimeError>>,
      Errors>;

  template <typename Arg, typename Errors, typename K>
  auto k(K k) && {
    return ExpectedToEventual(std::move(*this))
        .template k<
            Value_,
            tuple_types_union_t<
                std::tuple<
                    std::conditional_t<
                        check_errors_v<Error_>,
                        Error_,
                        RuntimeError>>,
                Errors>>(std::move(k));
  }

  using tl::expected<Value_, Error_>::expected;

  template <typename Downstream>
  static constexpr bool CanCompose = Downstream::ExpectsValue;

  using Expects = SingleValue;

  // Need explicit constructors for 'tl::expected', inherited
  // constructors are not sufficient.
  expected(const tl::expected<Value_, Error_>& that)
    : tl::expected<Value_, Error_>::expected(that) {}

  expected(tl::expected<Value_, Error_>&& that)
    : tl::expected<Value_, Error_>::expected(std::move(that)) {}
};

template <typename E>
using unexpected = tl::unexpected<E>;

using tl::make_unexpected;

////////////////////////////////////////////////////////////////////////

template <
    typename Left,
    typename T,
    typename E,
    std::enable_if_t<HasValueFrom<Left>::value, int> = 0>
[[nodiscard]] auto operator>>(Left left, tl::expected<T, E>&& expected) {
  return std::move(left)
      >> ExpectedToEventual(std::move(expected));
}

////////////////////////////////////////////////////////////////////////

template <
    typename Right,
    typename T,
    typename E,
    std::enable_if_t<HasValueFrom<Right>::value, int> = 0>
[[nodiscard]] auto operator>>(tl::expected<T, E>&& expected, Right right) {
  return ExpectedToEventual(std::move(expected))
      >> std::move(right);
}

////////////////////////////////////////////////////////////////////////

} // namespace eventuals

////////////////////////////////////////////////////////////////////////
