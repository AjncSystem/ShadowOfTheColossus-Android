// SPDX-License-Identifier: GPL-2.0-or-later
// Force-included (-include) into the FEX build only. The NDK r28 libc++ (LLVM 19) has no
// std::atomic_ref, which FEX uses for futexes, stats and code patching. This provides the subset
// FEX calls (load, store, exchange, compare_exchange_*, fetch_add/sub/and/or/xor) on top of the
// compiler __atomic builtins. It disables itself once the libc++ in use ships atomic_ref.
#pragma once

#if defined(__cplusplus) && __cplusplus >= 202002L
#include <version>

#if !defined(__cpp_lib_atomic_ref)
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace std {

namespace sotc_atomic_ref_detail {
constexpr int Order(memory_order o) {
  switch (o) {
  case memory_order_relaxed: return __ATOMIC_RELAXED;
  case memory_order_consume: return __ATOMIC_CONSUME;
  case memory_order_acquire: return __ATOMIC_ACQUIRE;
  case memory_order_release: return __ATOMIC_RELEASE;
  case memory_order_acq_rel: return __ATOMIC_ACQ_REL;
  default: return __ATOMIC_SEQ_CST;
  }
}
// Failure ordering derived from a single success ordering, as the standard specifies.
constexpr int FailOrder(memory_order o) {
  switch (o) {
  case memory_order_acq_rel: return __ATOMIC_ACQUIRE;
  case memory_order_release: return __ATOMIC_RELAXED;
  default: return Order(o);
  }
}
} // namespace sotc_atomic_ref_detail

template<typename T>
class atomic_ref {
  static_assert(is_trivially_copyable_v<T>, "atomic_ref requires a trivially copyable type");
  T* ptr_;

public:
  using value_type = T;
  static constexpr size_t required_alignment = alignof(T) > sizeof(T) ? alignof(T) : sizeof(T);
  static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), 0);

  explicit atomic_ref(T& obj) noexcept
    : ptr_ {__builtin_addressof(obj)} {}
  atomic_ref(const atomic_ref&) noexcept = default;
  atomic_ref& operator=(const atomic_ref&) = delete;

  bool is_lock_free() const noexcept {
    return __atomic_is_lock_free(sizeof(T), ptr_);
  }

  T load(memory_order o = memory_order_seq_cst) const noexcept {
    alignas(T) unsigned char buf[sizeof(T)];
    T* out = reinterpret_cast<T*>(buf);
    __atomic_load(ptr_, out, sotc_atomic_ref_detail::Order(o));
    return *out;
  }
  operator T() const noexcept {
    return load();
  }

  void store(T v, memory_order o = memory_order_seq_cst) const noexcept {
    __atomic_store(ptr_, __builtin_addressof(v), sotc_atomic_ref_detail::Order(o));
  }
  T operator=(T v) const noexcept {
    store(v);
    return v;
  }

  T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept {
    alignas(T) unsigned char buf[sizeof(T)];
    T* out = reinterpret_cast<T*>(buf);
    __atomic_exchange(ptr_, __builtin_addressof(v), out, sotc_atomic_ref_detail::Order(o));
    return *out;
  }

  bool compare_exchange_weak(T& expected, T desired, memory_order success, memory_order failure) const noexcept {
    return __atomic_compare_exchange(ptr_, __builtin_addressof(expected), __builtin_addressof(desired), true,
                                     sotc_atomic_ref_detail::Order(success), sotc_atomic_ref_detail::Order(failure));
  }
  bool compare_exchange_weak(T& expected, T desired, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_compare_exchange(ptr_, __builtin_addressof(expected), __builtin_addressof(desired), true,
                                     sotc_atomic_ref_detail::Order(o), sotc_atomic_ref_detail::FailOrder(o));
  }
  bool compare_exchange_strong(T& expected, T desired, memory_order success, memory_order failure) const noexcept {
    return __atomic_compare_exchange(ptr_, __builtin_addressof(expected), __builtin_addressof(desired), false,
                                     sotc_atomic_ref_detail::Order(success), sotc_atomic_ref_detail::Order(failure));
  }
  bool compare_exchange_strong(T& expected, T desired, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_compare_exchange(ptr_, __builtin_addressof(expected), __builtin_addressof(desired), false,
                                     sotc_atomic_ref_detail::Order(o), sotc_atomic_ref_detail::FailOrder(o));
  }

  // Integral and pointer arithmetic (FEX only uses these on integers).
  template<typename U = T>
  requires (is_integral_v<U> && !is_same_v<U, bool>)
  T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_add(ptr_, v, sotc_atomic_ref_detail::Order(o));
  }
  template<typename U = T>
  requires (is_integral_v<U> && !is_same_v<U, bool>)
  T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_sub(ptr_, v, sotc_atomic_ref_detail::Order(o));
  }
  template<typename U = T>
  requires (is_integral_v<U> && !is_same_v<U, bool>)
  T fetch_and(T v, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_and(ptr_, v, sotc_atomic_ref_detail::Order(o));
  }
  template<typename U = T>
  requires (is_integral_v<U> && !is_same_v<U, bool>)
  T fetch_or(T v, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_or(ptr_, v, sotc_atomic_ref_detail::Order(o));
  }
  template<typename U = T>
  requires (is_integral_v<U> && !is_same_v<U, bool>)
  T fetch_xor(T v, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_xor(ptr_, v, sotc_atomic_ref_detail::Order(o));
  }
};

} // namespace std

#endif // !__cpp_lib_atomic_ref
#endif // __cplusplus
