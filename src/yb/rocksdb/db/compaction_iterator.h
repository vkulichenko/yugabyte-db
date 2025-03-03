// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_ROCKSDB_DB_COMPACTION_ITERATOR_H
#define YB_ROCKSDB_DB_COMPACTION_ITERATOR_H

#pragma once

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "yb/rocksdb/db/compaction.h"
#include "yb/rocksdb/db/merge_helper.h"
#include "yb/rocksdb/compaction_filter.h"
#include "yb/rocksdb/util/log_buffer.h"

namespace rocksdb {

struct CompactionIteratorStats {
  // Compaction statistics
  int64_t num_record_drop_user = 0;
  int64_t num_record_drop_hidden = 0;
  int64_t num_record_drop_obsolete = 0;

  // Input statistics
  // TODO(noetzli): The stats are incomplete. They are lacking everything
  // consumed by MergeHelper.
  uint64_t num_input_records = 0;
  uint64_t num_input_deletion_records = 0;
  uint64_t num_input_corrupt_records = 0;
  uint64_t total_input_raw_key_bytes = 0;
  uint64_t total_input_raw_value_bytes = 0;
};

class CompactionIterator {
 public:
  CompactionIterator(InternalIterator* input, const Comparator* cmp,
                     MergeHelper* merge_helper, SequenceNumber last_sequence,
                     std::vector<SequenceNumber>* snapshots,
                     SequenceNumber earliest_write_conflict_snapshot,
                     bool expect_valid_internal_key,
                     Compaction* compaction = nullptr,
                     CompactionFilter* compaction_filter = nullptr,
                     LogBuffer* log_buffer = nullptr);

  void ResetRecordCounts();

  // Add live ranges to this iterator.
  // See live_key_ranges_stack_ comment for details.
  void AddLiveRanges(const std::vector<std::pair<Slice, Slice>>& ranges);

  // Seek to the beginning of the compaction iterator output.
  //
  // REQUIRED: Call only once.
  void SeekToFirst();

  // Produces the next record in the compaction.
  //
  // REQUIRED: SeekToFirst() has been called.
  void Next();

  // Getters
  const Slice& key() const { return key_; }
  const Slice& value() const { return value_; }
  const Status& status() const { return status_; }
  const ParsedInternalKey& ikey() const { return ikey_; }
  bool Valid() const { return valid_; }
  const Slice& user_key() const { return current_user_key_; }
  const CompactionIteratorStats& iter_stats() const { return iter_stats_; }

 private:
  // Processes the input stream to find the next output
  void NextFromInput();

  // Do last preparations before presenting the output to the callee. At this
  // point this only zeroes out the sequence number if possible for better
  // compression.
  void PrepareOutput();

  // Given a sequence number, return the sequence number of the
  // earliest snapshot that this sequence number is visible in.
  // The snapshots themselves are arranged in ascending order of
  // sequence numbers.
  // Employ a sequential search because the total number of
  // snapshots are typically small.
  inline SequenceNumber FindEarliestVisibleSnapshot(
      SequenceNumber in, SequenceNumber* prev_snapshot);

  InternalIterator* input_;
  const Comparator* cmp_;
  MergeHelper* merge_helper_;
  const std::vector<SequenceNumber>* snapshots_;
  const SequenceNumber earliest_write_conflict_snapshot_;
  bool expect_valid_internal_key_;
  Compaction* compaction_;
  CompactionFilter* compaction_filter_;
  LogBuffer* log_buffer_;
  bool bottommost_level_;
  bool valid_ = false;
  SequenceNumber visible_at_tip_;
  SequenceNumber earliest_snapshot_;
  SequenceNumber latest_snapshot_;
  bool ignore_snapshots_;

  // State
  //
  // Points to a copy of the current compaction iterator output (current_key_)
  // if valid_.
  Slice key_;
  // Points to the value in the underlying iterator that corresponds to the
  // current output.
  Slice value_;
  // The status is OK unless compaction iterator encounters a merge operand
  // while not having a merge operator defined.
  Status status_;
  // Stores the user key, sequence number and type of the current compaction
  // iterator output (or current key in the underlying iterator during
  // NextFromInput()).
  ParsedInternalKey ikey_;
  // Stores whether ikey_.user_key is valid. If set to false, the user key is
  // not compared against the current key in the underlying iterator.
  bool has_current_user_key_ = false;
  bool at_next_ = false;  // If false, the iterator
  // Holds a copy of the current compaction iterator output (or current key in
  // the underlying iterator during NextFromInput()).
  IterKey current_key_;
  Slice current_user_key_;
  SequenceNumber current_user_key_sequence_;
  SequenceNumber current_user_key_snapshot_;

  // True if the iterator has already returned a record for the current key.
  bool has_outputted_key_ = false;

  // truncated the value of the next key and output it without applying any
  // compaction rules.  This is used for outputting a put after a single delete.
  bool clear_and_output_next_key_ = false;

  MergeOutputIterator merge_out_iter_;
  std::string compaction_filter_value_;
  // "level_ptrs" holds indices that remember which file of an associated
  // level we were last checking during the last call to compaction->
  // KeyNotExistsBeyondOutputLevel(). This allows future calls to the function
  // to pick off where it left off since each subcompaction's key range is
  // increasing so a later call to the function must be looking for a key that
  // is in or beyond the last file checked during the previous call
  std::vector<size_t> level_ptrs_;
  CompactionIteratorStats iter_stats_;

  // Stores the disjoint live ranges of this tablet in user keyspace. Ranges at the back are
  // lexicographically first. Ranges are popped off the back of the stack as our iteration passes
  // them.
  std::vector<std::pair<Slice, Slice>> live_key_ranges_stack_;
};
}  // namespace rocksdb

#endif // YB_ROCKSDB_DB_COMPACTION_ITERATOR_H
