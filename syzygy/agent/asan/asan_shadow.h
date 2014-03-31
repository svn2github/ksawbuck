// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Implements an all-static class that manages shadow memory for ASAN.
#ifndef SYZYGY_AGENT_ASAN_ASAN_SHADOW_H_
#define SYZYGY_AGENT_ASAN_ASAN_SHADOW_H_

#include <string>

#include "base/basictypes.h"
#include "base/logging.h"

namespace agent {
namespace asan {

// An all-static class that manages the ASAN shadow memory.
class Shadow {
 public:
  // The granularity of the shadow memory.
  static const size_t kShadowGranularityLog = 3;
  static const size_t kShadowGranularity = 1 << kShadowGranularityLog;

  // One shadow byte for every 8 bytes in a 2G address space.
  // @note: This is dependent on the process NOT being large address aware.
  static const size_t kShadowSize = 1 << (31 - kShadowGranularityLog);

  // Set up the shadow memory.
  static void SetUp();

  // Tear down the shadow memory.
  static void TearDown();

  // The different markers we use to mark the shadow memory.
  enum ShadowMarker {
    kHeapAddressableByte = 0x00,
    kHeapNonAccessibleByteMask = 0xf0,
    kAsanMemoryByte = 0xf1,
    kInvalidAddress = 0xf2,
    kUserRedzone = 0xf3,
    kHeapBlockHeaderByte = 0xf4,
    kHeapLeftRedzone = 0xfa,
    kHeapRightRedzone = 0xfb,
    kAsanReservedByte = 0xfc,
    kHeapFreedByte = 0xfd,
  };

  // Poisons @p size bytes starting at @p addr with @p shadow_val value.
  // @pre addr + size mod 8 == 0.
  // @param address The starting address.
  // @param size The size of the memory to poison.
  // @param shadow_val The poison marker value.
  static void Poison(const void* addr, size_t size, ShadowMarker shadow_val);

  // Un-poisons @p size bytes starting at @p addr.
  // @pre addr mod 8 == 0 && size mod 8 == 0.
  // @param addr The starting address.
  // @param size The size of the memory to unpoison.
  static void Unpoison(const void* addr, size_t size);

  // Mark @p size bytes starting at @p addr as freed.
  // @param addr The starting address.
  // @param size The size of the memory to mark as freed.
  static void MarkAsFreed(const void* addr, size_t size);

  // Returns true iff the byte at @p addr is not poisoned.
  // @param addr The address that we want to check.
  // @returns true if this address is accessible, false otherwise.
  static bool IsAccessible(const void* addr);

  // Returns the ShadowMarker value for the byte at @p addr.
  // @param addr The address for which we want the ShadowMarker value.
  // @returns the ShadowMarker value for this address.
  static ShadowMarker GetShadowMarkerForAddress(const void* addr);

  // Appends a textual description of the shadow memory for @p addr to
  // @p output, including the values of the shadow bytes and a legend.
  // @param addr The address for which we want to get the textual description.
  // @param output The string in which we want to store this information.
  static void AppendShadowMemoryText(const void* addr, std::string* output);

  // Appends a textual description of the shadow memory for @p addr to
  // @p output. This only appends the values of the shadow bytes.
  // @param addr The address whose shadow memory is to be described.
  // @param output The string to be populated with the shadow memory
  //     information.
  static void AppendShadowArrayText(const void* addr, std::string* output);

  // Returns true iff the array starting at @p addr is terminated with
  // sizeof(@p type) null bytes within a contiguous accessible region of memory.
  // When returning true the length of the null-terminated array (including the
  // trailings zero) will be returned via @p size. When returning false the
  // offset of the invalid access will be returned via @p size.
  // @tparam type The type of the null terminated value, this determines the
  //     numbers of null bytes that we want to have at the end of the array.
  // @param addr The starting address of the array that we want to check.
  // @param max_size The maximum length to check (in bytes). Ignored if set to
  //     zero.
  // @param size Will receive the size (in bytes) of the array terminated with
  //     sizeof(type) bytes or the offset of the invalid access.
  // @returns true iff the array starting at @p addr is null terminated within a
  //     contiguous accessible region of memory, false otherwise.
  template<typename type>
  static bool GetNullTerminatedArraySize(const void* addr,
                                         size_t max_size,
                                         size_t* size);

  // Clones a shadow memory range from one location to another.
  // @pre src_pointer mod 8 == 0.
  // @pre dst_pointer mod 8 == 0.
  // @pre size mod 8 == 0.
  // @param src_pointer The starting address of the range to copy.
  // @param dst_pointer The destination where the copy should be made.
  // @param size The size of the range to copy.
  static void CloneShadowRange(const void* src_pointer,
                               void* dst_pointer,
                               size_t size);

  // Calculate the allocation size of a block by using the shadow memory.
  // @param mem A pointer inside the memory block for which we want to calculate
  //     the underlying allocation size.
  // @returns The underlying allocation size or 0 if it can't find a valid block
  //     at this address.
  // @note This function doesn't work for nested blocks.
  // TODO(sebmarchand): Add support for nested blocks.
  static size_t GetAllocSize(const uint8* mem);

  // Look in the shadow memory for the beginning of a block containing a given
  // address.
  // @param A pointer inside the memory block for which we want its beginning.
  // @returns The beginning of the block on success, false otherwise.
  // @note This function doesn't work for nested blocks.
  // TODO(sebmarchand): Add support for nested blocks.
  static const uint8* FindBlockBeginning(const uint8* mem);

  // Returns the block header for an ASan pointer.
  // @param asan_pointer The ASan pointer for which we want the block header
  //     pointer.
  // @returns A pointer to the block header of @p asan_pointer on success, NULL
  //     otherwise.
  static const uint8* AsanPointerToBlockHeader(const uint8* asan_pointer);

  // Checks if an address belongs to the left redzone of a block.
  // @param addr The address that we want to check.
  // @returns true if |addr| corresponds to a byte in the left redzone of a
  //     block, false otherwise.
  static bool IsLeftRedzone(const void* addr);

 protected:
  // Reset the shadow memory.
  static void Reset();

  // Appends a line of shadow byte text for the bytes ranging from
  // shadow_[index] to shadow_[index + 7], prefixed by @p prefix. If the index
  // @p bug_index is present in this range then its value will be surrounded by
  // brackets.
  static void AppendShadowByteText(const char *prefix,
                                   uintptr_t index,
                                   std::string* output,
                                   size_t bug_index);

  // The shadow memory.
  static uint8 shadow_[kShadowSize];
};

// A helper class to walk over the blocks contained in a given memory region.
// This uses only the metadata present in the shadow to identify the blocks.
class ShadowWalker {
 public:
  // Constructor.
  // @param lower_bound The lower bound of the region that this walker should
  //     cover in the actual memory.
  // @param upper_bound The upper bound of the region that this walker should
  //     cover in the actual memory.
  ShadowWalker(const uint8* lower_bound, const uint8* upper_bound);

  // Return the next block in this memory region.
  // @param block_begin Will receive the pointer to the next block in the region
  //     of interest. This will point to the beginning of a block, which may not
  //     necessarily be the block header depending on alignment requirements.
  //     This will be set to something >= @p upper_bound_ when the function
  //     returns false.
  // @return true if a block was found, false otherwise.
  bool Next(const uint8** block_begin);

  // Reset the walker to its initial state.
  void Reset();

 private:
  // Move |next_block_| to the next block.
  void Advance();

  // The bounds of the memory region for this walker.
  const uint8* lower_bound_;
  const uint8* upper_bound_;

  // The next block in the shadow, this will point to |upper_bound_| if there's
  // no next block.
  const uint8* next_block_;

  DISALLOW_COPY_AND_ASSIGN(ShadowWalker);
};

// Bring in the implementation of the templated functions.
#include "syzygy/agent/asan/asan_shadow_impl.h"

}  // namespace asan
}  // namespace agent

#endif  // SYZYGY_AGENT_ASAN_ASAN_SHADOW_H_
