#include "eventuals/catch.h"

#include <iostream>
#include <string>

#include "eventuals/conditional.h"
#include "eventuals/expected.h"
#include "eventuals/interrupt.h"
#include "eventuals/just.h"
#include "eventuals/raise.h"
#include "eventuals/then.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/promisify-for-test.h"

namespace eventuals::test {
namespace {

using testing::MockFunction;

TEST(CatchTest, RaisedRuntimeError) {
  auto e = []() {
    return Just(1)
        >> Raise(std::runtime_error("message"))
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return Then([]() {
                   return 100;
                 });
               })
               .raised<std::runtime_error>([](std::runtime_error&& error) {
                 EXPECT_STREQ(error.what(), "message");
                 return Just(100);
               });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(*e(), 100);
}

TEST(CatchTest, ChildException) {
  struct Error : public std::exception {
    const char* what() const noexcept override {
      return "child exception";
    }
  };

  auto e = []() {
    return Just(1)
        >> Raise(Error{})
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return Just(10);
               })
               .raised<std::exception>(
                   [](std::exception&& error) {
                     EXPECT_STREQ("child exception", error.what());
                     return Just(100);
                   });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(*e(), 100);
}

TEST(CatchTest, All) {
  auto e = []() {
    return Just(500)
        >> Raise(std::runtime_error("10"))
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 10;
               })
               .raised<std::underflow_error>([](std::underflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 10;
               })
               .all([](std::variant<std::runtime_error>&& error) {
                 EXPECT_STREQ(std::get<0>(error).what(), "10");
                 return 100;
               })
        >> Then([](int value) {
             return value;
           });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(*e(), 100);
}

TEST(CatchTest, UnexpectedRaise) {
  struct Error : public std::exception {
    const char* what() const noexcept override {
      return "child exception";
    }
  };

  auto f = []() -> expected<int, Error> {
    return make_unexpected(Error{});
  };

  auto e = [&]() {
    return f()
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 1;
               })
               .raised<Error>([](Error&& error) {
                 EXPECT_STREQ("child exception", error.what());
                 return 100;
               });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(*e(), 100);
}

TEST(CatchTest, UnexpectedAll) {
  struct Error : public std::exception {
    const char* what() const noexcept override {
      return "child exception";
    }
  };

  auto f = []() -> expected<int, Error> {
    return make_unexpected(Error{});
  };

  auto e = [&]() {
    return f()
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 1;
               })
               .raised<std::underflow_error>([](std::underflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 1;
               })
               .all([](auto&& error) {
                 EXPECT_STREQ(std::get<0>(error).what(), "child exception");

                 return 100;
               });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(*e(), 100);
}

TEST(CatchTest, NoExactHandler) {
  auto e = []() {
    return Just(1)
        >> Raise(std::string("error"))
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 1;
               })
               .raised<std::underflow_error>([](std::underflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return 1;
               });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<std::runtime_error>>);

  EXPECT_THROW(*e(), std::runtime_error);
}

TEST(CatchTest, ReRaise) {
  auto e = []() {
    return Just(1)
        >> Raise("10")
        >> Catch()
               .raised<std::runtime_error>([](std::runtime_error&& error) {
                 EXPECT_STREQ(error.what(), "10");
                 return Raise("1");
               })
               .all([](std::variant<std::runtime_error>&& error) {
                 ADD_FAILURE() << "Encountered an unexpected all";
                 return Just(100);
               })
        >> Then([](int) {
             return 200;
           })
        >> Catch()
               .raised<std::runtime_error>([](std::runtime_error&& error) {
                 EXPECT_STREQ(error.what(), "1");
                 return Just(10);
               })
        >> Then([](int value) {
             return value;
           });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(10, *e());
}

TEST(CatchTest, VoidPropagate) {
  auto e = []() {
    return Just("error")
        >> Then([](const char* i) {
             return;
           })
        >> Catch()
               .raised<std::exception>([](std::exception&& error) {
                 EXPECT_STREQ(error.what(), "error");
                 // MUST RETURN VOID HERE!
               })
        >> Then([](/* MUST TAKE VOID HERE! */) {
             return 100;
           });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  EXPECT_EQ(100, *e());
}

TEST(CatchTest, Interrupt) {
  auto e = []() {
    return Just(1)
        >> Raise(std::runtime_error("message"))
        >> Catch()
               .raised<std::overflow_error>([](std::overflow_error&& error) {
                 ADD_FAILURE() << "Encountered unexpected matched raised";
                 return Then([]() {
                   return 100;
                 });
               })
               .raised<std::runtime_error>([](std::runtime_error&& error) {
                 EXPECT_STREQ(error.what(), "message");
                 return Just(100);
               })
        >> Then([](int i) {
             return std::to_string(i);
           });
  };

  static_assert(
      eventuals::tuple_types_unordered_equals_v<
          decltype(e())::ErrorsFrom<void, std::tuple<>>,
          std::tuple<>>);

  auto [future, k] = PromisifyForTest(e());

  Interrupt interrupt;

  k.Register(interrupt);

  k.Start();

  EXPECT_EQ(future.get(), "100");
}

} // namespace
} // namespace eventuals::test
