/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/coro/ViaIfAsync.h>
#include <folly/result/try.h>

#if FOLLY_HAS_RESULT

namespace folly::coro {

/// `co_await_result` is the `result<T>` analog of the older `co_awaitTry`.
///
/// In a `folly::coro` async coroutine, use `co_await_result` like so:
///
///   result<int> res = co_await co_await_result(taskReturningInt());
///   if (auto* ex = get_exception<MyError>(res)) {
///     /* handle ex */
///   } else {
///     sum += co_await co_ready(res); // efficiently propagate unhandled error
///   }
///
/// Contrast that with related async coro vocabulary:
///  - `co_yield co_result(r)` from `Result.h` -- propagate `result<T>` or
///    `Try<T>` to the awaiter of the current coro.
///  - `auto& v = co_await co_ready(r)` from `Ready.h` -- given a `result<T>`,
///    unpack the value, or propagate any error to our awaiter.
///
/// The purpose of `co_await_result` is to handle errors from a child task via
/// `result<T>`, rather than through `try {} catch {}`.  Some reasons to do so:
///   - Your error-handling APIs (logging, retry, etc) use `result<T>`.
///   - You wish to avoid the ~microsecond cost of thrown exceptions,
///     applicable only when your error path is hot, and the child uses
///     `co_yield` instead of `throw` to propagate exceptions.

namespace detail {

// This reuses the `await_resume_try` machinery in the hope that the compiler
// will be able to optimize away the `Try` -> `result` conversion.  In some
// cases, this may incur an extra move-copy, so if this ends up being hot,
// adding a specialized `await_resume_result` interface may be justified.
template <typename Awaitable>
class ResultAwaiter {
 private:
  static_assert(is_awaitable_try<Awaitable&&>);

  using Awaiter = awaiter_type_t<Awaitable>;
  Awaiter awaiter_;

 public:
  explicit ResultAwaiter(Awaitable&& awaiter)
      : awaiter_(get_awaiter(static_cast<Awaitable&&>(awaiter))) {}

  // clang-format off
  auto await_ready() FOLLY_DETAIL_FORWARD_BODY(awaiter_.await_ready())

  template <typename Promise>
  auto await_suspend(coroutine_handle<Promise> coro)
      FOLLY_DETAIL_FORWARD_BODY(awaiter_.await_suspend(coro))

  auto await_resume()
      FOLLY_DETAIL_FORWARD_BODY(try_to_result(awaiter_.await_resume_try()))
  // clang-format on
};

template <typename T>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]] ResultAwaitable
    : public CommutativeWrapperAwaitable<ResultAwaitable, T> {
 public:
  using CommutativeWrapperAwaitable<ResultAwaitable, T>::
      CommutativeWrapperAwaitable;

  template <
      typename Self,
      std::enable_if_t<
          std::is_same_v<remove_cvref_t<Self>, ResultAwaitable>,
          int> = 0,
      typename T2 = like_t<Self, T>,
      std::enable_if_t<is_awaitable_v<T2>, int> = 0>
  friend ResultAwaiter<T2> operator co_await(Self && self) {
    return ResultAwaiter<T2>{static_cast<Self&&>(self).inner_};
  }

  using folly_private_noexcept_awaitable_t = std::true_type;
};

} // namespace detail

// IMPORTANT: If you need an `Awaitable&&` overload, you must bifurcate this
// API on `must_await_immediately_v`, see `co_awaitTry` for an example.
template <typename Awaitable>
detail::ResultAwaitable<Awaitable> co_await_result(
    [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT]] Awaitable awaitable) {
  return detail::ResultAwaitable<Awaitable>{
      mustAwaitImmediatelyUnsafeMover(std::move(awaitable))()};
}

} // namespace folly::coro

#endif
