// Copyright 2014 Google Inc. All Rights Reserved.
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
// Implementation of a heap manager that allocates blocks.

#ifndef SYZYGY_AGENT_ASAN_HEAP_MANAGERS_BLOCK_HEAP_MANAGER_H_
#define SYZYGY_AGENT_ASAN_HEAP_MANAGERS_BLOCK_HEAP_MANAGER_H_

#include <windows.h>

#include <unordered_map>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "syzygy/agent/asan/block_utils.h"
#include "syzygy/agent/asan/error_info.h"
#include "syzygy/agent/asan/heap.h"
#include "syzygy/agent/asan/heap_manager.h"
#include "syzygy/agent/asan/quarantine.h"
#include "syzygy/agent/asan/stack_capture_cache.h"
#include "syzygy/agent/asan/heaps/zebra_block_heap.h"
#include "syzygy/agent/asan/memory_notifiers/null_memory_notifier.h"
#include "syzygy/agent/asan/quarantines/sharded_quarantine.h"
#include "syzygy/common/asan_parameters.h"

namespace agent {
namespace asan {

// Forward declarations.
class AsanRuntime;

namespace heap_managers {

// A block heap manager is an implementation of a heap manager that allocates
// and manages blocks.
//
// It is responsible for maintaining the state of the shadow memory, and thus
// updating it when a block's state changes. This also takes care of maintaining
// a quarantine of freed blocks.
//
// When the user requests a new heap he will receive a pointer to a
// SimpleBlockHeap by default. However the goal of this manager is to
// automatically choose the most appropriate heap for a given allocation so the
// actual heap that serves an allocation can be different from the one returned
// to the user.
//
// The zebra heap is created once, when enabled for the first time, with a
// specified size. It can't be resized after creation. Disabling the zebra
// heap only disables allocations on it, deallocations will continue to work.
// TODO(sebmarchand): Plug in other heaps, like the zebra heap and the large
//     block heap.
// TODO(sebmarchand): Add page protection support.
class BlockHeapManager : public HeapManagerInterface {
 public:
  // Constructor.
  // @param runtime The runtime instance to use.
  explicit BlockHeapManager(AsanRuntime* runtime);

  // Destructor.
  virtual ~BlockHeapManager();

  // @name HeapManagerInterface functions.
  // @{
  virtual HeapId CreateHeap();
  virtual bool DestroyHeap(HeapId heap_id);
  virtual void* Allocate(HeapId heap_id, size_t bytes);
  virtual bool Free(HeapId heap_id, void* alloc);
  virtual size_t Size(HeapId heap_id, const void* alloc);
  virtual void Lock(HeapId heap_id);
  virtual void Unlock(HeapId heap_id);
  // @}

  // Set the parameters of this heap manager.
  // @param trailer_padding_size The trailer padding size, in bytes.
  void set_parameters(const common::AsanParameters& parameters);

  // Get the parameters.
  common::AsanParameters parameters() {
    return parameters_;
  }

 protected:
  // The type of quarantine that we use internally.
  typedef quarantines::ShardedQuarantine<BlockHeader*,
                                         GetTotalBlockSizeFunctor,
                                         GetBlockHashFunctor,
                                         kQuarantineDefaultShardingFactor>
      ShardedBlockQuarantine;

  // A map associating a block heap with its underlying heap.
  typedef std::unordered_map<BlockHeapInterface*, HeapInterface*>
      UnderlyingHeapMap;

  // A map associating a block heap with the quarantine it will use. Many heaps
  // may share a single quarantine.
  typedef std::unordered_map<BlockHeapInterface*, BlockQuarantineInterface*>
      HeapQuarantineMap;

  // Propagates the parameters to the appropriate modules.
  // @note This function is responsible for acquiring lock_ when necessary.
  void PropagateParameters();

  // Destroy a heap and flush its quarantine. If this heap has an underlying
  // heap it'll also destroy it. All the block belonging to this heap that are
  // in the quarantine will be freed.
  //
  // @param heap The heap to destroy.
  // @param quarantine The quarantine of this heap.
  // @returns true on success, false otherwise.
  // @note The heap pointer will be invalid if this function succeeds.
  // @note This must be called under lock_.
  bool DestroyHeapUnlocked(BlockHeapInterface* heap,
                           BlockQuarantineInterface* quarantine);

  // If the quarantine of a heap is over its maximum size, trim it down until
  // it's below the limit. If parameters_.quarantine_size is 0 then
  // then quarantine is flushed.
  // @param quarantine The quarantine to trim.
  // TODO(peterssen): Change the 0-size contract. The quarantine 'contract'
  //    establish that when the size is 0, it means unlimited, this is rather
  //    awkward since trimming with size 0 should flush the quarantine.
  void TrimQuarantine(BlockQuarantineInterface* quarantine);

  // Free a block that might be corrupt. If the block is corrupt first reports
  // an error before safely releasing the block.
  // @param block_info The information about this block.
  // @returns true if the block has been successfully freed, false otherwise.
  bool FreePotentiallyCorruptBlock(BlockInfo* block_info);

  // Free a corrupt block. This takes care of cleaning its metadata before
  // trying to free it.
  // @param block_info The information about this block.
  // @returns true if the block has been successfully freed, false otherwise.
  bool FreeCorruptBlock(BlockInfo* block_info);

  // Free an allocated block. This should be called when a block is removed from
  // the quarantine or directly freed. This takes care of updating the shadow
  // memory and releasing the resources acquired by this block (like its stack
  // traces). The block should either not be corrupt or cleaned from its unsafe
  // metadata.
  // @param block_info The information about this block.
  // @returns true on success, false otherwise.
  bool FreePristineBlock(BlockInfo* block_info);

  // Clears the metadata of a corrupt block. After calling this function the
  // block can safely be passed to FreeBlock.
  // @param block_info The information about this block.
  void ClearCorruptBlockMetadata(BlockInfo* block_info);

  // Sets the callback that this heap will invoke when heap corruption is
  // encountered.
  // @param heap_error_callback The callback to be invoked when heap
  //     corruption is encountered.
  void SetHeapErrorCallback(HeapErrorCallback heap_error_callback) {
    heap_error_callback_ = heap_error_callback;
  }

  // Reports a heap error via the heap error callback. This is for originating
  // errors that are detected while performing operations on a heap metadata.
  // Read/write errors are detected outside of the manager, and query the heap
  // for information about the error itself.
  // @param address The address that was being accessed/manipulating when the
  //     error was detected.
  // @param kind The type of error encountered.
  void ReportHeapError(void* address, BadAccessKind kind);

  // The runtime instance to use to report the errors.
  // TODO(sebmarchand): We don't really need to know about the runtime instance,
  //     remove this and use the SetHeapErrorCallback once this has been plugged
  //     into AsanRuntime.
  AsanRuntime* runtime_;

  // Protects concurrent access to the heap manager internals.
  base::Lock lock_;

  // Contains the heaps owned by this manager.
  HeapQuarantineMap heaps_;  // Under lock_.

  // The quarantine shared by the heaps created by this manager.
  ShardedBlockQuarantine shared_quarantine_;

  // Map the block heaps to their underlying heap.
  UnderlyingHeapMap underlying_heaps_map_;  // Under lock_.

  // The parameters of this heap manager.
  common::AsanParameters parameters_;

  // The callback this manager uses to expose internal state errors. These are
  // caused by uninstrumented code (system libraries, etc), thus aren't caught
  // at their source. Catching their side effect as early as possible allows the
  // recovery of some useful debugging information.
  HeapErrorCallback heap_error_callback_;

  // The heap that gets used for the unguarded allocations.
  scoped_ptr<HeapInterface> unguarded_allocation_heap_;

  // Hold the single ZebraBlockHeap instance used by this heap manager.
  // The lifetime management of the zebra heap is provided by the
  // HeapQuarantineMap, this is simply a useful pointer for finding the
  // zebra heap directly.
  heaps::ZebraBlockHeap* zebra_block_heap_;

  // Memory notifier used as a temporal workaround for the ZebraBlockHeap.
  // TODO(peterssen): Remove when the memory notifier is implemented.
  memory_notifiers::NullMemoryNotifier null_memory_notifier;

 private:
  DISALLOW_COPY_AND_ASSIGN(BlockHeapManager);
};

}  // namespace heap_managers
}  // namespace asan
}  // namespace agent

#endif  // SYZYGY_AGENT_ASAN_HEAP_MANAGERS_BLOCK_HEAP_MANAGER_H_
