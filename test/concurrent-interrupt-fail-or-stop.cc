#include <deque>
#include <string>
#include <vector>

#include "eventuals/callback.h"
#include "eventuals/collect.h"
#include "eventuals/eventual.h"
#include "eventuals/interrupt.h"
#include "eventuals/iterate.h"
#include "eventuals/let.h"
#include "eventuals/map.h"
#include "eventuals/terminal.h"
#include "test/concurrent.h"

using eventuals::Callback;
using eventuals::Collect;
using eventuals::Eventual;
using eventuals::Interrupt;
using eventuals::Iterate;
using eventuals::Let;
using eventuals::Map;
using eventuals::Terminate;

// Tests that 'Concurrent()' and 'ConcurrentOrdered()' defers to the
// eventuals on how to handle interrupts and in this case one of the
// eventuals will stop and one will fail so the result will be either
// a fail or stop. 'Fail' for 'ConcurrentOrdered()'.
TYPED_TEST(ConcurrentTypedTest, InterruptFailOrStop) {
  std::deque<Callback<>> callbacks;

  auto e = [&]() {
    return Iterate({1, 2})
        | this->ConcurrentOrConcurrentOrdered([&]() {
            return Map(Let([&](int& i) {
              return Eventual<std::string>()
                  .interruptible()
                  .start([&](auto& k, Interrupt::Handler& handler) mutable {
                    if (i == 1) {
                      handler.Install([&k]() {
                        k.Stop();
                      });
                    } else {
                      handler.Install([&k]() {
                        k.Fail("error");
                      });
                    }
                    callbacks.emplace_back([]() {});
                  });
            }));
          })
        | Collect<std::vector<std::string>>();
  };

  auto [future, k] = Terminate(e());

  Interrupt interrupt;

  k.Register(interrupt);

  k.Start();

  ASSERT_EQ(2, callbacks.size());

  EXPECT_EQ(
      std::future_status::timeout,
      future.wait_for(std::chrono::seconds(0)));

  interrupt.Trigger();

  // NOTE: expecting "any" throwable here depending on whether the
  // eventual that stopped or failed was completed first.
  // Expecting 'std::exception_ptr' for 'ConcurrentOrdered'.
  if constexpr (std::is_same_v<TypeParam, ConcurrentType>) {
    EXPECT_ANY_THROW(future.get());
  } else {
    EXPECT_THROW(future.get(), std::exception_ptr);
  }
}
