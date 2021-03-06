#include "sort.hpp"

#include <algorithm>

#if defined(OMEGA_H_USE_CUDA)
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <thrust/device_ptr.h>
#include <thrust/sort.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#elif defined(OMEGA_H_USE_OPENMP)
#include <omp.h>
#include "intel_sort/parallel_stable_sort.hpp"
#include "intel_sort/pss_common.hpp"
#else
#endif

namespace Omega_h {

template <typename T, typename Comp>
void parallel_sort(T* b, T* e, Comp c) {
#if defined(OMEGA_H_USE_CUDA)
  auto bptr = thrust::device_ptr<T>(b);
  auto eptr = thrust::device_ptr<T>(e);
  thrust::stable_sort(bptr, eptr, c);
#elif defined(OMEGA_H_USE_OPENMP)
  pss::parallel_stable_sort(b, e, c);
#else
  std::stable_sort(b, e, c);
#endif
}

template <typename T, Int N>
struct CompareKeySets {
  T const* keys_;
  CompareKeySets(T const* keys) : keys_(keys) {}
  INLINE bool operator()(const LO& a, const LO& b) const {
    for (Int i = 0; i < N; ++i) {
      T x = keys_[a * N + i];
      T y = keys_[b * N + i];
      if (x != y) return x < y;
    }
    return false;
  }
};

template <Int N, typename T>
static LOs sort_by_keys_tmpl(Read<T> keys) {
  CHECK(keys.size() % N == 0);
  Write<LO> perm(keys.size() / N, 0, 1);
  LO* begin = perm.data();
  LO* end = perm.data() + perm.size();
  T const* keyptr = keys.data();
  parallel_sort<LO, CompareKeySets<T, N>>(
      begin, end, CompareKeySets<T, N>(keyptr));
  return perm;
}

template <typename T>
LOs sort_by_keys(Read<T> keys, Int width) {
  switch (width) {
    case 1:
      return sort_by_keys_tmpl<1>(keys);
    case 2:
      return sort_by_keys_tmpl<2>(keys);
    case 3:
      return sort_by_keys_tmpl<3>(keys);
  }
  NORETURN(LOs());
}

#define INST(T) template LOs sort_by_keys(Read<T> keys, Int width);
INST(LO)
INST(GO)
#undef INST

}  // end namespace Omega_h
