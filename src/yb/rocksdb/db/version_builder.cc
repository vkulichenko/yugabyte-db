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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/db/version_builder.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/internal_stats.h"
#include "yb/rocksdb/db/table_cache.h"
#include "yb/rocksdb/db/version_set.h"
#include "yb/rocksdb/table/table_reader.h"

#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/status_log.h"

namespace rocksdb {

bool NewestFirstBySeqNo(FileMetaData* a, FileMetaData* b) {
  if (a->smallest.seqno != b->smallest.seqno) {
    return a->smallest.seqno > b->smallest.seqno;
  }
  if (a->largest.seqno != b->largest.seqno) {
    return a->largest.seqno > b->largest.seqno;
  }
  // Break ties by file number
  return a->fd.GetNumber() > b->fd.GetNumber();
}

namespace {
bool BySmallestKey(FileMetaData* a, FileMetaData* b,
                   const InternalKeyComparator* cmp) {
  int r = cmp->Compare(a->smallest.key, b->smallest.key);
  if (r != 0) {
    return (r < 0);
  }
  // Break ties by file number
  return (a->fd.GetNumber() < b->fd.GetNumber());
}
}  // namespace

class VersionBuilder::Rep {
 private:
  // Helper to sort files_ in v
  // kLevel0 -- NewestFirstBySeqNo
  // kLevelNon0 -- BySmallestKey
  struct FileComparator {
    enum SortMethod { kLevel0 = 0, kLevelNon0 = 1, } sort_method;
    InternalKeyComparatorPtr internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      switch (sort_method) {
        case kLevel0:
          return NewestFirstBySeqNo(f1, f2);
        case kLevelNon0:
          return BySmallestKey(f1, f2, internal_comparator.get());
      }
      assert(false);
      return false;
    }
  };

  struct LevelState {
    std::unordered_set<uint64_t> deleted_files;
    // Map from file number to file meta data.
    std::unordered_map<uint64_t, FileMetaData*> added_files;
  };

  const EnvOptions& env_options_;
  Logger* info_log_;
  TableCache* table_cache_;
  VersionStorageInfo* base_vstorage_;
  LevelState* levels_;
  FileComparator level_zero_cmp_;
  FileComparator level_nonzero_cmp_;

 public:
  Rep(const EnvOptions& env_options, Logger* info_log, TableCache* table_cache,
      VersionStorageInfo* base_vstorage)
      : env_options_(env_options),
        info_log_(info_log),
        table_cache_(table_cache),
        base_vstorage_(base_vstorage) {
    levels_ = new LevelState[base_vstorage_->num_levels()];
    level_zero_cmp_.sort_method = FileComparator::kLevel0;
    level_nonzero_cmp_.sort_method = FileComparator::kLevelNon0;
    level_nonzero_cmp_.internal_comparator =
        base_vstorage_->InternalComparator();
  }

  ~Rep() {
    for (int level = 0; level < base_vstorage_->num_levels(); level++) {
      const auto& added = levels_[level].added_files;
      for (auto& pair : added) {
        UnrefFile(pair.second);
      }
    }

    delete[] levels_;
  }

  void UnrefFile(FileMetaData* f) {
    if (f->Unref(table_cache_)) {
      delete f;
    }
  }

  void CheckConsistency(VersionStorageInfo* vstorage) {
#ifndef NDEBUG
    // make sure the files are sorted correctly
    for (int level = 0; level < vstorage->num_levels(); level++) {
      auto& level_files = vstorage->LevelFiles(level);
      for (size_t i = 1; i < level_files.size(); i++) {
        auto f1 = level_files[i - 1];
        auto f2 = level_files[i];
        if (level == 0) {
          assert(level_zero_cmp_(f1, f2));
          assert(f1->largest.seqno > f2->largest.seqno ||
                 // We can have multiple files with seqno = 0 as a result of
                 // using DB::AddFile()
                 (f1->largest.seqno == 0 && f2->largest.seqno == 0));
        } else {
          assert(level_nonzero_cmp_(f1, f2));

          // Make sure there is no overlap in levels > 0
          if (vstorage->InternalComparator()->Compare(f1->largest.key,
                                                      f2->smallest.key) >= 0) {
            fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                    f1->largest.key.DebugString().c_str(),
                    f2->smallest.key.DebugString().c_str());
            abort();
          }
        }
      }
    }
#endif
  }

  void CheckConsistencyForDeletes(VersionEdit* edit, uint64_t number,
                                  int level) {
#ifndef NDEBUG
    // a file to be deleted better exist in the previous version
    bool found = false;
    for (int l = 0; !found && l < base_vstorage_->num_levels(); l++) {
      const std::vector<FileMetaData*>& base_files =
          base_vstorage_->LevelFiles(l);
      for (size_t i = 0; i < base_files.size(); i++) {
        FileMetaData* f = base_files[i];
        if (f->fd.GetNumber() == number) {
          found = true;
          break;
        }
      }
    }
    // if the file did not exist in the previous version, then it
    // is possibly moved from lower level to higher level in current
    // version
    for (int l = level + 1; !found && l < base_vstorage_->num_levels(); l++) {
      auto& level_added = levels_[l].added_files;
      auto got = level_added.find(number);
      if (got != level_added.end()) {
        found = true;
        break;
      }
    }

    // maybe this file was added in a previous edit that was Applied
    if (!found) {
      auto& level_added = levels_[level].added_files;
      auto got = level_added.find(number);
      if (got != level_added.end()) {
        found = true;
      }
    }
    LOG_IF(DFATAL, !found) << yb::Format(
        "$0SST file not found: $1", info_log_ ? info_log_->Prefix() : "", number);
#endif
  }

  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    CheckConsistency(base_vstorage_);

    // Delete files
    const VersionEdit::DeletedFileSet& del = edit->GetDeletedFiles();
    for (const auto& del_file : del) {
      const auto level = del_file.first;
      const auto number = del_file.second;
      levels_[level].deleted_files.insert(number);
      CheckConsistencyForDeletes(edit, number, level);

      auto exising = levels_[level].added_files.find(number);
      if (exising != levels_[level].added_files.end()) {
        UnrefFile(exising->second);
        levels_[level].added_files.erase(number);
      }
    }

    // Add new files
    for (const auto& new_file : edit->GetNewFiles()) {
      const int level = new_file.first;
      FileMetaData* f = new FileMetaData(new_file.second);
      f->refs = 1;

      assert(levels_[level].added_files.find(f->fd.GetNumber()) ==
             levels_[level].added_files.end());
      levels_[level].deleted_files.erase(f->fd.GetNumber());
      levels_[level].added_files[f->fd.GetNumber()] = f;
    }
  }

  // Save the current state in *v.
  void SaveTo(VersionStorageInfo* vstorage) {
    CheckConsistency(base_vstorage_);
    CheckConsistency(vstorage);

    for (int level = 0; level < base_vstorage_->num_levels(); level++) {
      const auto& cmp = (level == 0) ? level_zero_cmp_ : level_nonzero_cmp_;
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      const auto& base_files = base_vstorage_->LevelFiles(level);
      auto base_iter = base_files.begin();
      auto base_end = base_files.end();
      const auto& unordered_added_files = levels_[level].added_files;
      vstorage->Reserve(level,
                        base_files.size() + unordered_added_files.size());

      // Sort added files for the level.
      std::vector<FileMetaData*> added_files;
      added_files.reserve(unordered_added_files.size());
      for (const auto& pair : unordered_added_files) {
        added_files.push_back(pair.second);
      }
      std::sort(added_files.begin(), added_files.end(), cmp);

#ifndef NDEBUG
      FileMetaData* prev_file = nullptr;
#endif

      for (const auto& added : added_files) {
#ifndef NDEBUG
        if (level > 0 && prev_file != nullptr) {
          assert(base_vstorage_->InternalComparator()->Compare(
                     prev_file->smallest.key, added->smallest.key) <= 0);
        }
        prev_file = added;
#endif

        // Add all smaller files listed in base_
        for (auto bpos = std::upper_bound(base_iter, base_end, added, cmp);
             base_iter != bpos; ++base_iter) {
          MaybeAddFile(vstorage, level, *base_iter);
        }

        MaybeAddFile(vstorage, level, added);
      }

      // Add remaining base files
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(vstorage, level, *base_iter);
      }
    }

    CheckConsistency(vstorage);
  }

  void LoadTableHandlers(InternalStats* internal_stats, int max_threads) {
    assert(table_cache_ != nullptr);
    // <file metadata, level>
    std::vector<std::pair<FileMetaData*, int>> files_meta;
    for (int level = 0; level < base_vstorage_->num_levels(); level++) {
      for (auto& file_meta_pair : levels_[level].added_files) {
        auto* file_meta = file_meta_pair.second;
        assert(!file_meta->table_reader_handle);
        files_meta.emplace_back(file_meta, level);
      }
    }

    std::atomic<size_t> next_file_meta_idx(0);
    std::function<void()> load_handlers_func = [&]() {
      while (true) {
        size_t file_idx = next_file_meta_idx.fetch_add(1);
        if (file_idx >= files_meta.size()) {
          break;
        }

        auto* file_meta = files_meta[file_idx].first;
        int level = files_meta[file_idx].second;
        CHECK_OK(table_cache_->FindTable(env_options_,
                                         base_vstorage_->InternalComparator(),
                                         file_meta->fd, &file_meta->table_reader_handle,
                                         kDefaultQueryId,
                                         false /*no_io */, true /* record_read_stats */,
                                         internal_stats->GetFileReadHist(level)));
        if (file_meta->table_reader_handle != nullptr) {
          // Load table_reader
          file_meta->fd.table_reader = table_cache_->GetTableReaderFromHandle(
              file_meta->table_reader_handle);
        }
      }
    };

    if (max_threads <= 1) {
      load_handlers_func();
    } else {
      std::vector<std::thread> threads;
      for (int i = 0; i < max_threads; i++) {
        threads.emplace_back(load_handlers_func);
      }

      for (auto& t : threads) {
        t.join();
      }
    }
  }

  void MaybeAddFile(VersionStorageInfo* vstorage, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->fd.GetNumber()) > 0) {
      // f is to-be-delected table file
      vstorage->RemoveCurrentStats(f);
    } else {
      vstorage->AddFile(level, f, info_log_);
    }
  }
};

VersionBuilder::VersionBuilder(const EnvOptions& env_options,
                               TableCache* table_cache,
                               VersionStorageInfo* base_vstorage,
                               Logger* info_log)
    : rep_(new Rep(env_options, info_log, table_cache, base_vstorage)) {}
VersionBuilder::~VersionBuilder() { delete rep_; }
void VersionBuilder::CheckConsistency(VersionStorageInfo* vstorage) {
  rep_->CheckConsistency(vstorage);
}
void VersionBuilder::CheckConsistencyForDeletes(VersionEdit* edit,
                                                uint64_t number, int level) {
  rep_->CheckConsistencyForDeletes(edit, number, level);
}
void VersionBuilder::Apply(VersionEdit* edit) { rep_->Apply(edit); }
void VersionBuilder::SaveTo(VersionStorageInfo* vstorage) {
  rep_->SaveTo(vstorage);
}
void VersionBuilder::LoadTableHandlers(InternalStats* internal_stats,
                                       int max_threads) {
  rep_->LoadTableHandlers(internal_stats, max_threads);
}
void VersionBuilder::MaybeAddFile(VersionStorageInfo* vstorage, int level,
                                  FileMetaData* f) {
  rep_->MaybeAddFile(vstorage, level, f);
}

}  // namespace rocksdb
