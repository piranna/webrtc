/*
 *  Copyright 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// This file contains Macros for creating proxies for webrtc MediaStream and
// PeerConnection classes.
// TODO(deadbeef): Move this to pc/; this is part of the implementation.

// The proxied objects are initialized with either one or two thread
// objects that operations can be proxied to: The primary and secondary
// threads.
// In common usage, the primary thread will be the PeerConnection's
// signaling thread, and the secondary thread will be either the
// PeerConnection's worker thread or the PeerConnection's network thread.

//
// Example usage:
//
// class TestInterface : public rtc::RefCountInterface {
//  public:
//   std::string FooA() = 0;
//   std::string FooB(bool arg1) const = 0;
//   std::string FooC(bool arg1) = 0;
//  };
//
// Note that return types can not be a const reference.
//
// class Test : public TestInterface {
// ... implementation of the interface.
// };
//
// BEGIN_PROXY_MAP(Test)
//   PROXY_PRIMARY_THREAD_DESTRUCTOR()
//   PROXY_METHOD0(std::string, FooA)
//   PROXY_CONSTMETHOD1(std::string, FooB, arg1)
//   PROXY_SECONDARY_METHOD1(std::string, FooC, arg1)
// END_PROXY_MAP()
//
// Where the destructor and first two methods are invoked on the primary
// thread, and the third is invoked on the secondary thread.
//
// The proxy can be created using
//
//   TestProxy::Create(Thread* signaling_thread, Thread* worker_thread,
//                     TestInterface*).
//
// The variant defined with BEGIN_PRIMARY_PROXY_MAP is unaware of
// the secondary thread, and invokes all methods on the primary thread.
//
// The variant defined with BEGIN_OWNED_PROXY_MAP does not use
// refcounting, and instead just takes ownership of the object being proxied.

#ifndef API_PROXY_H_
#define API_PROXY_H_

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "api/scoped_refptr.h"
#include "api/task_queue/queued_task.h"
#include "api/task_queue/task_queue_base.h"
#include "rtc_base/event.h"
#include "rtc_base/message_handler.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/system/rtc_export.h"
#include "rtc_base/thread.h"

namespace rtc {
class Location;
}

namespace webrtc {
namespace proxy_internal {
RTC_EXPORT void TraceApiCall(const char* class_name, const char* method_name);
}  // namespace proxy_internal

template <typename R>
class ReturnType {
 public:
  template <typename C, typename M, typename... Args>
  void Invoke(C* c, M m, Args&&... args) {
    r_ = (c->*m)(std::forward<Args>(args)...);
  }

  R moved_result() { return std::move(r_); }

 private:
  R r_;
};

template <>
class ReturnType<void> {
 public:
  template <typename C, typename M, typename... Args>
  void Invoke(C* c, M m, Args&&... args) {
    (c->*m)(std::forward<Args>(args)...);
  }

  void moved_result() {}
};

template <typename C, typename R, typename... Args>
class MethodCall : public QueuedTask {
 public:
  typedef R (C::*Method)(Args...);
  MethodCall(C* c, Method m, Args&&... args)
      : c_(c),
        m_(m),
        args_(std::forward_as_tuple(std::forward<Args>(args)...)) {}

  R Marshal(const rtc::Location& posted_from, rtc::Thread* t) {
    if (t->IsCurrent()) {
      Invoke(std::index_sequence_for<Args...>());
    } else {
      t->PostTask(std::unique_ptr<QueuedTask>(this));
      event_.Wait(rtc::Event::kForever);
    }
    return r_.moved_result();
  }

 private:
  bool Run() override {
    Invoke(std::index_sequence_for<Args...>());
    event_.Set();
    return false;
  }

  template <size_t... Is>
  void Invoke(std::index_sequence<Is...>) {
    r_.Invoke(c_, m_, std::move(std::get<Is>(args_))...);
  }

  C* c_;
  Method m_;
  ReturnType<R> r_;
  std::tuple<Args&&...> args_;
  rtc::Event event_;
};

template <typename C, typename R, typename... Args>
class ConstMethodCall : public QueuedTask {
 public:
  typedef R (C::*Method)(Args...) const;
  ConstMethodCall(const C* c, Method m, Args&&... args)
      : c_(c),
        m_(m),
        args_(std::forward_as_tuple(std::forward<Args>(args)...)) {}

  R Marshal(const rtc::Location& posted_from, rtc::Thread* t) {
    if (t->IsCurrent()) {
      Invoke(std::index_sequence_for<Args...>());
    } else {
      t->PostTask(std::unique_ptr<QueuedTask>(this));
      event_.Wait(rtc::Event::kForever);
    }
    return r_.moved_result();
  }

 private:
  bool Run() override {
    Invoke(std::index_sequence_for<Args...>());
    event_.Set();
    return false;
  }

  template <size_t... Is>
  void Invoke(std::index_sequence<Is...>) {
    r_.Invoke(c_, m_, std::move(std::get<Is>(args_))...);
  }

  const C* c_;
  Method m_;
  ReturnType<R> r_;
  std::tuple<Args&&...> args_;
  rtc::Event event_;
};

#define PROXY_STRINGIZE_IMPL(x) #x
#define PROXY_STRINGIZE(x) PROXY_STRINGIZE_IMPL(x)

// Helper macros to reduce code duplication.
#define PROXY_MAP_BOILERPLATE(c)                          \
  template <class INTERNAL_CLASS>                         \
  class c##ProxyWithInternal;                             \
  typedef c##ProxyWithInternal<c##Interface> c##Proxy;    \
  template <class INTERNAL_CLASS>                         \
  class c##ProxyWithInternal : public c##Interface {      \
   protected:                                             \
    typedef c##Interface C;                               \
    const char* class_name_ = PROXY_STRINGIZE(c);         \
                                                          \
   public:                                                \
    const INTERNAL_CLASS* internal() const { return c_; } \
    INTERNAL_CLASS* internal() { return c_; }

// clang-format off
// clang-format would put the semicolon alone,
// leading to a presubmit error (cpplint.py)
#define END_PROXY_MAP() \
  };
// clang-format on

#define PRIMARY_PROXY_MAP_BOILERPLATE(c)                               \
 protected:                                                            \
  c##ProxyWithInternal(rtc::Thread* primary_thread, INTERNAL_CLASS* c) \
      : primary_thread_(primary_thread), c_(c) {}                      \
                                                                       \
 private:                                                              \
  mutable rtc::Thread* primary_thread_;

#define SECONDARY_PROXY_MAP_BOILERPLATE(c)                               \
 protected:                                                              \
  c##ProxyWithInternal(rtc::Thread* primary_thread,                      \
                       rtc::Thread* secondary_thread, INTERNAL_CLASS* c) \
      : primary_thread_(primary_thread),                                 \
        secondary_thread_(secondary_thread),                             \
        c_(c) {}                                                         \
                                                                         \
 private:                                                                \
  mutable rtc::Thread* primary_thread_;                                  \
  mutable rtc::Thread* secondary_thread_;

// Note that the destructor is protected so that the proxy can only be
// destroyed via RefCountInterface.
#define REFCOUNTED_PROXY_MAP_BOILERPLATE(c)            \
 protected:                                            \
  ~c##ProxyWithInternal() {                            \
    MethodCall<c##ProxyWithInternal, void> call(       \
        this, &c##ProxyWithInternal::DestroyInternal); \
    call.Marshal(RTC_FROM_HERE, destructor_thread());  \
  }                                                    \
                                                       \
 private:                                              \
  void DestroyInternal() { c_ = nullptr; }             \
  rtc::scoped_refptr<INTERNAL_CLASS> c_;

// Note: This doesn't use a unique_ptr, because it intends to handle a corner
// case where an object's deletion triggers a callback that calls back into
// this proxy object. If relying on a unique_ptr to delete the object, its
// inner pointer would be set to null before this reentrant callback would have
// a chance to run, resulting in a segfault.
#define OWNED_PROXY_MAP_BOILERPLATE(c)                 \
 public:                                               \
  ~c##ProxyWithInternal() {                            \
    MethodCall<c##ProxyWithInternal, void> call(       \
        this, &c##ProxyWithInternal::DestroyInternal); \
    call.Marshal(RTC_FROM_HERE, destructor_thread());  \
  }                                                    \
                                                       \
 private:                                              \
  void DestroyInternal() { delete c_; }                \
  INTERNAL_CLASS* c_;

#define BEGIN_PRIMARY_PROXY_MAP(c)                                         \
  PROXY_MAP_BOILERPLATE(c)                                                 \
  PRIMARY_PROXY_MAP_BOILERPLATE(c)                                         \
  REFCOUNTED_PROXY_MAP_BOILERPLATE(c)                                      \
 public:                                                                   \
  static rtc::scoped_refptr<c##ProxyWithInternal> Create(                  \
      rtc::Thread* primary_thread, INTERNAL_CLASS* c) {                    \
    return rtc::make_ref_counted<c##ProxyWithInternal>(primary_thread, c); \
  }

#define BEGIN_PROXY_MAP(c)                                                   \
  PROXY_MAP_BOILERPLATE(c)                                                   \
  SECONDARY_PROXY_MAP_BOILERPLATE(c)                                         \
  REFCOUNTED_PROXY_MAP_BOILERPLATE(c)                                        \
 public:                                                                     \
  static rtc::scoped_refptr<c##ProxyWithInternal> Create(                    \
      rtc::Thread* primary_thread, rtc::Thread* secondary_thread,            \
      INTERNAL_CLASS* c) {                                                   \
    return rtc::make_ref_counted<c##ProxyWithInternal>(primary_thread,       \
                                                       secondary_thread, c); \
  }

#define BEGIN_OWNED_PROXY_MAP(c)                                   \
  PROXY_MAP_BOILERPLATE(c)                                         \
  SECONDARY_PROXY_MAP_BOILERPLATE(c)                               \
  OWNED_PROXY_MAP_BOILERPLATE(c)                                   \
 public:                                                           \
  static std::unique_ptr<c##Interface> Create(                     \
      rtc::Thread* primary_thread, rtc::Thread* secondary_thread,  \
      std::unique_ptr<INTERNAL_CLASS> c) {                         \
    return std::unique_ptr<c##Interface>(new c##ProxyWithInternal( \
        primary_thread, secondary_thread, c.release()));           \
  }

#define PROXY_PRIMARY_THREAD_DESTRUCTOR()                            \
 private:                                                            \
  rtc::Thread* destructor_thread() const { return primary_thread_; } \
                                                                     \
 public:  // NOLINTNEXTLINE

#define PROXY_SECONDARY_THREAD_DESTRUCTOR()                            \
 private:                                                              \
  rtc::Thread* destructor_thread() const { return secondary_thread_; } \
                                                                       \
 public:  // NOLINTNEXTLINE

#define PROXY_METHOD0(r, method)                                        \
  r method() override {                                                 \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r> call(c_, &C::method);                              \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_CONSTMETHOD0(r, method)                                   \
  r method() const override {                                           \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    ConstMethodCall<C, r> call(c_, &C::method);                         \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_METHOD1(r, method, t1)                                    \
  r method(t1 a1) override {                                            \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1> call(c_, &C::method, std::move(a1));           \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_CONSTMETHOD1(r, method, t1)                               \
  r method(t1 a1) const override {                                      \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    ConstMethodCall<C, r, t1> call(c_, &C::method, std::move(a1));      \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_METHOD2(r, method, t1, t2)                                \
  r method(t1 a1, t2 a2) override {                                     \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1, t2> call(c_, &C::method, std::move(a1),        \
                                  std::move(a2));                       \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_METHOD3(r, method, t1, t2, t3)                            \
  r method(t1 a1, t2 a2, t3 a3) override {                              \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1, t2, t3> call(c_, &C::method, std::move(a1),    \
                                      std::move(a2), std::move(a3));    \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                \
  }

#define PROXY_METHOD4(r, method, t1, t2, t3, t4)                         \
  r method(t1 a1, t2 a2, t3 a3, t4 a4) override {                        \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method));  \
    MethodCall<C, r, t1, t2, t3, t4> call(c_, &C::method, std::move(a1), \
                                          std::move(a2), std::move(a3),  \
                                          std::move(a4));                \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                 \
  }

#define PROXY_METHOD5(r, method, t1, t2, t3, t4, t5)                         \
  r method(t1 a1, t2 a2, t3 a3, t4 a4, t5 a5) override {                     \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method));      \
    MethodCall<C, r, t1, t2, t3, t4, t5> call(c_, &C::method, std::move(a1), \
                                              std::move(a2), std::move(a3),  \
                                              std::move(a4), std::move(a5)); \
    return call.Marshal(RTC_FROM_HERE, primary_thread_);                     \
  }

// Define methods which should be invoked on the secondary thread.
#define PROXY_SECONDARY_METHOD0(r, method)                              \
  r method() override {                                                 \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r> call(c_, &C::method);                              \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_CONSTMETHOD0(r, method)                         \
  r method() const override {                                           \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    ConstMethodCall<C, r> call(c_, &C::method);                         \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_METHOD1(r, method, t1)                          \
  r method(t1 a1) override {                                            \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1> call(c_, &C::method, std::move(a1));           \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_CONSTMETHOD1(r, method, t1)                     \
  r method(t1 a1) const override {                                      \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    ConstMethodCall<C, r, t1> call(c_, &C::method, std::move(a1));      \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_METHOD2(r, method, t1, t2)                      \
  r method(t1 a1, t2 a2) override {                                     \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1, t2> call(c_, &C::method, std::move(a1),        \
                                  std::move(a2));                       \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_CONSTMETHOD2(r, method, t1, t2)                 \
  r method(t1 a1, t2 a2) const override {                               \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    ConstMethodCall<C, r, t1, t2> call(c_, &C::method, std::move(a1),   \
                                       std::move(a2));                  \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_METHOD3(r, method, t1, t2, t3)                  \
  r method(t1 a1, t2 a2, t3 a3) override {                              \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method)); \
    MethodCall<C, r, t1, t2, t3> call(c_, &C::method, std::move(a1),    \
                                      std::move(a2), std::move(a3));    \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);              \
  }

#define PROXY_SECONDARY_CONSTMETHOD3(r, method, t1, t2)                   \
  r method(t1 a1, t2 a2, t3 a3) const override {                          \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method));   \
    ConstMethodCall<C, r, t1, t2, t3> call(c_, &C::method, std::move(a1), \
                                           std::move(a2), std::move(a3)); \
    return call.Marshal(RTC_FROM_HERE, secondary_thread_);                \
  }

// For use when returning purely const state (set during construction).
// Use with caution. This method should only be used when the return value will
// always be the same.
#define BYPASS_PROXY_CONSTMETHOD0(r, method)                                \
  r method() const override {                                               \
    proxy_internal::TraceApiCall(class_name_, PROXY_STRINGIZE(method));     \
    return c_->method();                                                    \
  }

}  // namespace webrtc

#endif  //  API_PROXY_H_
