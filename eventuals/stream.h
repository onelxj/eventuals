#pragma once

// TODO(benh): infinite recursion via thread-local storage.
//
// TODO(benh): 'Stop()' on stream should break infinite recursion
// (figure out how to embed a std::atomic).
//
// TODO(benh): disallow calling 'Next()' after calling 'Done()'.
//
// TODO(benh): disallow calling 'Emit()' before call to 'Next()'.

#include <memory>
#include <variant>

#include "eventuals/eventual.h"

////////////////////////////////////////////////////////////////////////

namespace eventuals {

////////////////////////////////////////////////////////////////////////

struct TypeErasedStream {
  virtual ~TypeErasedStream() = default;
  virtual void Next() = 0;
  virtual void Done() = 0;
};

////////////////////////////////////////////////////////////////////////

struct _Stream final {
  // Helper that distinguishes when a stream's continuation needs to be
  // invoked (versus the stream being invoked as a continuation itself).
  template <typename S_, typename K_, typename Arg_>
  struct StreamK final {
    S_* stream_ = nullptr;
    K_* k_ = nullptr;
    std::optional<
        std::conditional_t<
            std::is_void_v<Arg_>,
            std::monostate,
            std::conditional_t<
                std::is_reference_v<Arg_>,
                std::reference_wrapper<std::remove_reference_t<Arg_>>,
                Arg_>>>
        arg_;

    void Begin() {
      stream_->previous_->Continue([this]() {
        k_->Begin(*stream_);
      });
    }

    template <typename... Args>
    void Fail(Args&&... args) {
      stream_->previous_->Continue(
          [&]() {
            k_->Fail(std::forward<Args>(args)...);
          },
          [&]() {
            // TODO(benh): avoid allocating on heap by storing args in
            // pre-allocated buffer based on composing with Errors.
            using Tuple = std::tuple<decltype(k_), Args...>;
            auto tuple = std::make_unique<Tuple>(
                k_,
                std::forward<Args>(args)...);
            return [tuple = std::move(tuple)]() mutable {
              std::apply(
                  [](auto* k_, auto&&... args) {
                    k_->Fail(std::forward<decltype(args)>(args)...);
                  },
                  std::move(*tuple));
            };
          });
    }

    void Stop() {
      stream_->previous_->Continue([this]() {
        k_->Stop();
      });
    }

    template <typename... Args>
    void Emit(Args&&... args) {
      stream_->previous_->Continue(
          [&]() {
            k_->Body(std::forward<Args>(args)...);
          },
          [&]() {
            static_assert(
                sizeof...(args) == 0 || sizeof...(args) == 1,
                "'emit()' only supports 0 or 1 argument, but found > 1");

            static_assert(std::is_void_v<Arg_> || sizeof...(args) == 1);

            if constexpr (!std::is_void_v<Arg_>) {
              arg_.emplace(std::forward<Args>(args)...);
            }

            return [this]() {
              if constexpr (!std::is_void_v<Arg_>) {
                if constexpr (std::is_reference_v<Arg_>) {
                  k_->Body(arg_->get());
                } else {
                  k_->Body(std::move(*arg_));
                }
              } else {
                k_->Body();
              }
            };
          });
    }

    void Ended() {
      stream_->previous_->Continue([this]() {
        k_->Ended();
      });
    }
  };

  template <
      typename K_,
      typename Context_,
      typename Begin_,
      typename Next_,
      typename Done_,
      typename Fail_,
      typename Stop_,
      bool Interruptible_,
      typename Value_,
      typename... Errors_>
  struct Continuation final : public TypeErasedStream {
    // NOTE: explicit constructor because inheriting 'TypeErasedStream'.
    Continuation(
        K_ k,
        Context_ context,
        Begin_ begin,
        Next_ next,
        Done_ done,
        Fail_ fail,
        Stop_ stop)
      : context_(std::move(context)),
        begin_(std::move(begin)),
        next_(std::move(next)),
        done_(std::move(done)),
        fail_(std::move(fail)),
        stop_(std::move(stop)),
        k_(std::move(k)) {}

    Continuation(Continuation&& that) = default;

    Continuation& operator=(Continuation&& that) {
      // TODO(benh): lambdas don't have an 'operator=()' until C++20 so
      // we have to effectively do a "reset" and "emplace" (as though it
      // was stored in a 'std::optional' but without the overhead of
      // optionals everywhere).
      this->~Continuation();
      new (this) Continuation(std::move(that));

      return *this;
    }

    ~Continuation() override = default;

    template <typename... Args>
    void Start(Args&&... args) {
      previous_ = Scheduler::Context::Get();

      streamk_.stream_ = this;
      streamk_.k_ = &k_;

      if constexpr (IsUndefined<Begin_>::value) {
        streamk_.Begin(std::forward<Args>(args)...);
      } else {
        if constexpr (!IsUndefined<Context_>::value && Interruptible_) {
          CHECK(handler_);
          begin_(context_, streamk_, *handler_, std::forward<Args>(args)...);
        } else if constexpr (!IsUndefined<Context_>::value && !Interruptible_) {
          begin_(context_, streamk_, std::forward<Args>(args)...);
        } else if constexpr (IsUndefined<Context_>::value && Interruptible_) {
          CHECK(handler_);
          begin_(streamk_, *handler_, std::forward<Args>(args)...);
        } else {
          begin_(streamk_, std::forward<Args>(args)...);
        }
      }
    }

    template <typename... Args>
    void Fail(Args&&... args) {
      if constexpr (IsUndefined<Fail_>::value) {
        k_.Fail(std::forward<Args>(args)...);
      } else if constexpr (IsUndefined<Context_>::value) {
        fail_(k_, std::forward<Args>(args)...);
      } else {
        fail_(context_, k_, std::forward<Args>(args)...);
      }
    }

    void Stop() {
      if constexpr (IsUndefined<Stop_>::value) {
        k_.Stop();
      } else if constexpr (IsUndefined<Context_>::value) {
        stop_(k_);
      } else {
        stop_(context_, k_);
      }
    }

    void Register(Interrupt& interrupt) {
      k_.Register(interrupt);

      if constexpr (Interruptible_) {
        handler_.emplace(&interrupt);
      }
    }

    void Next() override {
      static_assert(
          !IsUndefined<Next_>::value,
          "Undefined 'next' (and no default)");

      previous_->Continue([this]() {
        if constexpr (IsUndefined<Context_>::value) {
          next_(streamk_);
        } else {
          next_(context_, streamk_);
        }
      });
    }

    void Done() override {
      previous_->Continue([this]() {
        if constexpr (IsUndefined<Done_>::value) {
          k_.Ended();
        } else if constexpr (IsUndefined<Context_>::value) {
          done_(streamk_);
        } else {
          done_(context_, streamk_);
        }
      });
    }

    Context_ context_;
    Begin_ begin_;
    Next_ next_;
    Done_ done_;
    Fail_ fail_;
    Stop_ stop_;

    Scheduler::Context* previous_ = nullptr;

    StreamK<Continuation, K_, Value_> streamk_;

    std::optional<Interrupt::Handler> handler_;

    // NOTE: we store 'k_' as the _last_ member so it will be
    // destructed _first_ and thus we won't have any use-after-delete
    // issues during destruction of 'k_' if it holds any references or
    // pointers to any (or within any) of the above members.
    K_ k_;
  };

  template <
      typename Context_,
      typename Begin_,
      typename Next_,
      typename Done_,
      typename Fail_,
      typename Stop_,
      bool Interruptible_,
      typename Value_,
      typename... Errors_>
  struct Builder final {
    template <typename Arg>
    using ValueFrom = Value_;

    template <
        bool Interruptible,
        typename Value,
        typename... Errors,
        typename Context,
        typename Begin,
        typename Next,
        typename Done,
        typename Fail,
        typename Stop>
    static auto create(
        Context context,
        Begin begin,
        Next next,
        Done done,
        Fail fail,
        Stop stop) {
      return Builder<
          Context,
          Begin,
          Next,
          Done,
          Fail,
          Stop,
          Interruptible,
          Value,
          Errors...>{
          std::move(context),
          std::move(begin),
          std::move(next),
          std::move(done),
          std::move(fail),
          std::move(stop)};
    }

    template <typename Arg, typename K>
    auto k(K k) && {
      return Continuation<
          K,
          Context_,
          Begin_,
          Next_,
          Done_,
          Fail_,
          Stop_,
          Interruptible_,
          Value_,
          Errors_...>(
          std::move(k),
          std::move(context_),
          std::move(begin_),
          std::move(next_),
          std::move(done_),
          std::move(fail_),
          std::move(stop_));
    }

    template <typename Context>
    auto context(Context context) && {
      static_assert(IsUndefined<Context_>::value, "Duplicate 'context'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context),
          std::move(begin_),
          std::move(next_),
          std::move(done_),
          std::move(fail_),
          std::move(stop_));
    }

    template <typename Begin>
    auto begin(Begin begin) && {
      static_assert(IsUndefined<Begin_>::value, "Duplicate 'begin'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context_),
          std::move(begin),
          std::move(next_),
          std::move(done_),
          std::move(fail_),
          std::move(stop_));
    }

    template <typename Next>
    auto next(Next next) && {
      static_assert(IsUndefined<Next_>::value, "Duplicate 'next'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context_),
          std::move(begin_),
          std::move(next),
          std::move(done_),
          std::move(fail_),
          std::move(stop_));
    }

    template <typename Done>
    auto done(Done done) && {
      static_assert(IsUndefined<Done_>::value, "Duplicate 'done'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context_),
          std::move(begin_),
          std::move(next_),
          std::move(done),
          std::move(fail_),
          std::move(stop_));
    }

    template <typename Fail>
    auto fail(Fail fail) && {
      static_assert(IsUndefined<Fail_>::value, "Duplicate 'fail'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context_),
          std::move(begin_),
          std::move(next_),
          std::move(done_),
          std::move(fail),
          std::move(stop_));
    }

    template <typename Stop>
    auto stop(Stop stop) && {
      static_assert(IsUndefined<Stop_>::value, "Duplicate 'stop'");
      return create<Interruptible_, Value_, Errors_...>(
          std::move(context_),
          std::move(begin_),
          std::move(next_),
          std::move(done_),
          std::move(fail_),
          std::move(stop));
    }

    auto interruptible() && {
      static_assert(!Interruptible_, "Already 'interruptible'");
      return create<true, Value_, Errors_...>(
          std::move(context_),
          std::move(begin_),
          std::move(next_),
          std::move(done_),
          std::move(fail_),
          std::move(stop_));
    }

    Context_ context_;
    Begin_ begin_;
    Next_ next_;
    Done_ done_;
    Fail_ fail_;
    Stop_ stop_;
  };
};

////////////////////////////////////////////////////////////////////////

template <typename Value, typename... Errors>
auto Stream() {
  return _Stream::Builder<
      Undefined,
      Undefined,
      Undefined,
      Undefined,
      Undefined,
      Undefined,
      false,
      Value,
      Errors...>{};
}

////////////////////////////////////////////////////////////////////////

} // namespace eventuals

////////////////////////////////////////////////////////////////////////
