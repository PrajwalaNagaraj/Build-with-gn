// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdio.h>

#include "base/allocator/buildflags.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/process/process_metrics.h"
#include "base/system/sys_info.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(USE_TCMALLOC)
namespace {

using std::min;

#ifdef NDEBUG
// We wrap malloc and free in noinline functions to ensure that we test the real
// implementation of the allocator. Otherwise, the compiler may specifically
// recognize the calls to malloc and free in our tests and optimize them away.
NOINLINE void* TCMallocDoMallocForTest(size_t size) {
  return malloc(size);
}

NOINLINE void TCMallocDoFreeForTest(void* ptr) {
  free(ptr);
}
#endif

// Fill a buffer of the specified size with a predetermined pattern
static void Fill(unsigned char* buffer, int n) {
  for (int i = 0; i < n; i++) {
    buffer[i] = (i & 0xff);
  }
}

// Check that the specified buffer has the predetermined pattern
// generated by Fill()
static bool Valid(unsigned char* buffer, int n) {
  for (int i = 0; i < n; i++) {
    if (buffer[i] != (i & 0xff)) {
      return false;
    }
  }
  return true;
}

// Return the next interesting size/delta to check.  Returns -1 if no more.
static int NextSize(int size) {
  if (size < 100)
    return size + 1;

  if (size < 100000) {
    // Find next power of two
    int power = 1;
    while (power < size)
      power <<= 1;

    // Yield (power-1, power, power+1)
    if (size < power - 1)
      return power - 1;

    if (size == power - 1)
      return power;

    CHECK_EQ(size, power);
    return power + 1;
  }
  return -1;
}

static void TestCalloc(size_t n, size_t s, bool ok) {
  char* p = reinterpret_cast<char*>(calloc(n, s));
  if (!ok) {
    EXPECT_EQ(nullptr, p) << "calloc(n, s) should not succeed";
  } else {
    EXPECT_NE(reinterpret_cast<void*>(NULL), p)
        << "calloc(n, s) should succeed";
    for (size_t i = 0; i < n * s; i++) {
      EXPECT_EQ('\0', p[i]);
    }
    free(p);
  }
}

bool IsLowMemoryDevice() {
  return base::SysInfo::AmountOfPhysicalMemory() <= 256LL * 1024 * 1024;
}

}  // namespace

TEST(TCMallocTest, Malloc) {
  // Try allocating data with a bunch of alignments and sizes
  for (int size = 1; size < 1048576; size *= 2) {
    unsigned char* ptr = reinterpret_cast<unsigned char*>(malloc(size));
    // Should be 2 byte aligned
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(ptr) & 1);
    Fill(ptr, size);
    EXPECT_TRUE(Valid(ptr, size));
    free(ptr);
  }
}

TEST(TCMallocTest, Calloc) {
  TestCalloc(0, 0, true);
  TestCalloc(0, 1, true);
  TestCalloc(1, 1, true);
  TestCalloc(1 << 10, 0, true);
  TestCalloc(1 << 20, 0, true);
  TestCalloc(0, 1 << 10, true);
  TestCalloc(0, 1 << 20, true);
  TestCalloc(1 << 20, 2, true);
  TestCalloc(2, 1 << 20, true);
  TestCalloc(1000, 1000, true);
}

#ifdef NDEBUG
// This makes sure that reallocing a small number of bytes in either
// direction doesn't cause us to allocate new memory. Tcmalloc in debug mode
// does not follow this.
TEST(TCMallocTest, ReallocSmallDelta) {
  int start_sizes[] = {100, 1000, 10000, 100000};
  int deltas[] = {1, -2, 4, -8, 16, -32, 64, -128};

  for (unsigned s = 0; s < sizeof(start_sizes) / sizeof(*start_sizes); ++s) {
    void* p = malloc(start_sizes[s]);
    ASSERT_TRUE(p);
    // The larger the start-size, the larger the non-reallocing delta.
    for (unsigned d = 0; d < s * 2; ++d) {
      void* new_p = realloc(p, start_sizes[s] + deltas[d]);
      ASSERT_EQ(p, new_p);  // realloc should not allocate new memory
    }
    // Test again, but this time reallocing smaller first.
    for (unsigned d = 0; d < s * 2; ++d) {
      void* new_p = realloc(p, start_sizes[s] - deltas[d]);
      ASSERT_EQ(p, new_p);  // realloc should not allocate new memory
    }
    free(p);
  }
}
#endif

TEST(TCMallocTest, Realloc) {
  for (int src_size = 0; src_size >= 0; src_size = NextSize(src_size)) {
    for (int dst_size = 0; dst_size >= 0; dst_size = NextSize(dst_size)) {
      unsigned char* src = reinterpret_cast<unsigned char*>(malloc(src_size));
      Fill(src, src_size);
      unsigned char* dst =
          reinterpret_cast<unsigned char*>(realloc(src, dst_size));
      EXPECT_TRUE(Valid(dst, min(src_size, dst_size)));
      Fill(dst, dst_size);
      EXPECT_TRUE(Valid(dst, dst_size));
      if (dst != nullptr)
        free(dst);
    }
  }

  // The logic below tries to allocate kNumEntries * 9000 ~= 130 MB of memory.
  // This would cause the test to crash on low memory devices with no VM
  // overcommit (e.g., chromecast).
  if (IsLowMemoryDevice())
    return;

  // Now make sure realloc works correctly even when we overflow the
  // packed cache, so some entries are evicted from the cache.
  // The cache has 2^12 entries, keyed by page number.
  const int kNumEntries = 1 << 14;
  int** p = reinterpret_cast<int**>(malloc(sizeof(*p) * kNumEntries));
  int sum = 0;
  for (int i = 0; i < kNumEntries; i++) {
    // no page size is likely to be bigger than 8192?
    p[i] = reinterpret_cast<int*>(malloc(8192));
    p[i][1000] = i;  // use memory deep in the heart of p
  }
  for (int i = 0; i < kNumEntries; i++) {
    p[i] = reinterpret_cast<int*>(realloc(p[i], 9000));
  }
  for (int i = 0; i < kNumEntries; i++) {
    sum += p[i][1000];
    free(p[i]);
  }
  EXPECT_EQ(kNumEntries / 2 * (kNumEntries - 1), sum);  // assume kNE is even
  free(p);
}

#ifdef NDEBUG
TEST(TCMallocFreeTest, BadPointerInFirstPageOfTheLargeObject) {
  const size_t kPageSize = base::GetPageSize();
  char* p =
      reinterpret_cast<char*>(TCMallocDoMallocForTest(10 * kPageSize + 1));
  for (unsigned offset = 1; offset < kPageSize; offset <<= 1) {
    ASSERT_DEATH(TCMallocDoFreeForTest(p + offset),
                 "Pointer is not pointing to the start of a span");
  }
  TCMallocDoFreeForTest(p);
}

// TODO(ssid): Fix flakiness and enable the test, crbug.com/571549.
TEST(TCMallocFreeTest, DISABLED_BadPageAlignedPointerInsideLargeObject) {
  const size_t kPageSize = base::GetPageSize();
  const size_t kMaxSize = 10 * kPageSize;
  char* p = reinterpret_cast<char*>(TCMallocDoMallocForTest(kMaxSize + 1));

  for (unsigned offset = kPageSize; offset < kMaxSize; offset += kPageSize) {
    // Only the first and last page of a span are in heap map. So for others
    // tcmalloc will give a general error of invalid pointer.
    ASSERT_DEATH(TCMallocDoFreeForTest(p + offset), "");
  }
  ASSERT_DEATH(TCMallocDoFreeForTest(p + kMaxSize),
               "Pointer is not pointing to the start of a span");
  TCMallocDoFreeForTest(p);
}

TEST(TCMallocFreeTest, DoubleFreeLargeObject) {
  const size_t kMaxSize = 10 * base::GetPageSize();
  char* p = reinterpret_cast<char*>(TCMallocDoMallocForTest(kMaxSize + 1));
  ASSERT_DEATH(TCMallocDoFreeForTest(p); TCMallocDoFreeForTest(p),
               "Object was not in-use");
}

TEST(TCMallocFreeTest, DoubleFreeSmallObject) {
  const size_t kPageSize = base::GetPageSize();
  for (size_t size = 1; size <= kPageSize; size <<= 1) {
    char* p = reinterpret_cast<char*>(TCMallocDoMallocForTest(size));
    ASSERT_DEATH(TCMallocDoFreeForTest(p); TCMallocDoFreeForTest(p),
                 "Circular loop in list detected");
  }
}
#endif  // NDEBUG

#endif