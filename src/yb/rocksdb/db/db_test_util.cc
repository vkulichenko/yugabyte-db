// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
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

#include "yb/rocksdb/db/db_test_util.h"

#include "yb/encryption/header_manager_impl.h"
#include "yb/encryption/universe_key_manager.h"

#include "yb/rocksdb/util/logging.h"

#include "yb/rocksutil/rocksdb_encrypted_file_factory.h"
#include "yb/rocksutil/yb_rocksdb_logger.h"

#include "yb/util/random_util.h"
#include "yb/util/status_log.h"

namespace rocksdb {

// Special Env used to delay background operations

SpecialEnv::SpecialEnv(Env* base)
    : EnvWrapper(base),
      rnd_(301),
      sleep_counter_(this),
      addon_time_(0),
      time_elapse_only_sleep_(false),
      no_sleep_(false) {
  delay_sstable_sync_.store(false, std::memory_order_release);
  drop_writes_.store(false, std::memory_order_release);
  no_space_.store(false, std::memory_order_release);
  non_writable_.store(false, std::memory_order_release);
  count_random_reads_ = false;
  count_sequential_reads_ = false;
  manifest_sync_error_.store(false, std::memory_order_release);
  manifest_write_error_.store(false, std::memory_order_release);
  log_write_error_.store(false, std::memory_order_release);
  random_file_open_counter_.store(0, std::memory_order_relaxed);
  log_write_slowdown_ = 0;
  bytes_written_ = 0;
  sync_counter_ = 0;
  non_writeable_rate_ = 0;
  new_writable_count_ = 0;
  non_writable_count_ = 0;
  table_write_callback_ = nullptr;
}

const string DBHolder::kKeyId = "key_id";
const string DBHolder::kKeyFile = "universe_key_file";

DBHolder::DBHolder(const std::string path, bool encryption_enabled)
    : option_config_(kDefault),
      mem_env_(!getenv("MEM_ENV") ? nullptr : new MockEnv(Env::Default())),
      env_(new SpecialEnv(mem_env_ ? mem_env_ : Env::Default())) {
  if (encryption_enabled) {
    CreateEncryptedEnv();
  }
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
  dbname_ = test::TmpDir(env_) + path;
  alternative_wal_dir_ = dbname_ + "/wal";
  alternative_db_log_dir_ = dbname_ + "/db_log_dir";
  auto options = CurrentOptions();
  auto delete_options = options;
  delete_options.wal_dir = alternative_wal_dir_;
  WARN_NOT_OK(DestroyDB(dbname_, delete_options), "Cleanup failed " + dbname_);
  // Destroy it for not alternative WAL dir is used.
  WARN_NOT_OK(DestroyDB(dbname_, options), "Cleanup failed " + dbname_);
  db_ = nullptr;
  Reopen(options);
  Random::GetTLSInstance()->Reset(0xdeadbeef);
}

DBHolder::~DBHolder() {
  Env::Default()->CleanupFile(kKeyFile);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  rocksdb::SyncPoint::GetInstance()->LoadDependency({});
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  Close();
  Options options;
  options.db_paths.emplace_back(dbname_, 0);
  options.db_paths.emplace_back(dbname_ + "_2", 0);
  options.db_paths.emplace_back(dbname_ + "_3", 0);
  options.db_paths.emplace_back(dbname_ + "_4", 0);
  EXPECT_OK(DestroyDB(dbname_, options));
  delete env_;
}

void DBHolder::CreateEncryptedEnv() {
  auto bytes = yb::RandomBytes(32);
  yb::Slice key(bytes.data(), bytes.size());
  auto status = yb::WriteStringToFile(yb::Env::Default(), key, kKeyFile);
  if (!status.ok()) {
    LOG(FATAL) << "Could not write slice to file:" << status.ToString();
  }

  auto res = yb::encryption::UniverseKeyManager::FromKey(kKeyId, key);
  if (!res.ok()) {
    LOG(FATAL) << "Could not get key from bytes:" << res.status().ToString();
  }
  universe_key_manager_ = std::move(*res);
  encrypted_env_ = yb::NewRocksDBEncryptedEnv(
      yb::encryption::DefaultHeaderManager(universe_key_manager_.get()));
  delete env_;
  env_ = new rocksdb::SpecialEnv(encrypted_env_.get());
}

bool DBHolder::ShouldSkipOptions(int option_config, int skip_mask) {
#ifdef ROCKSDB_LITE
    // These options are not supported in ROCKSDB_LITE
  if (option_config == kHashSkipList ||
      option_config == kPlainTableFirstBytePrefix ||
      option_config == kPlainTableCappedPrefix ||
      option_config == kPlainTableCappedPrefixNonMmap ||
      option_config == kPlainTableAllBytesPrefix ||
      option_config == kVectorRep || option_config == kHashLinkList ||
      option_config == kUniversalCompaction ||
      option_config == kUniversalCompactionMultiLevel ||
      option_config == kUniversalSubcompactions ||
      option_config == kFIFOCompaction ||
      option_config == kConcurrentSkipList) {
    return true;
    }
#endif

    if ((skip_mask & kSkipDeletesFilterFirst) &&
        option_config == kDeletesFilterFirst) {
      return true;
    }
    if ((skip_mask & kSkipUniversalCompaction) &&
        (option_config == kUniversalCompaction ||
         option_config == kUniversalCompactionMultiLevel)) {
      return true;
    }
    if ((skip_mask & kSkipMergePut) && option_config == kMergePut) {
      return true;
    }
    if ((skip_mask & kSkipNoSeekToLast) &&
        (option_config == kHashLinkList || option_config == kHashSkipList)) {
      return true;
    }
    if ((skip_mask & kSkipPlainTable) &&
        (option_config == kPlainTableAllBytesPrefix ||
         option_config == kPlainTableFirstBytePrefix ||
         option_config == kPlainTableCappedPrefix ||
         option_config == kPlainTableCappedPrefixNonMmap)) {
      return true;
    }
    if ((skip_mask & kSkipHashIndex) &&
        (option_config == kBlockBasedTableWithPrefixHashIndex ||
         option_config == kBlockBasedTableWithWholeKeyHashIndex)) {
      return true;
    }
    if ((skip_mask & kSkipFIFOCompaction) && option_config == kFIFOCompaction) {
      return true;
    }
    if ((skip_mask & kSkipMmapReads) && option_config == kWalDirAndMmapReads) {
      return true;
    }
    return false;
}

// Switch to a fresh database with the next option configuration to
// test.  Return false if there are no more configurations to test.
bool DBHolder::ChangeOptions(int skip_mask) {
  for (option_config_++; option_config_ < kEnd; option_config_++) {
    if (ShouldSkipOptions(option_config_, skip_mask)) {
      continue;
    }
    break;
  }

  if (option_config_ >= kEnd) {
    Destroy(last_options_);
    return false;
  } else {
    auto options = CurrentOptions();
    options.create_if_missing = true;
    DestroyAndReopen(options);
    return true;
  }
}

// Switch between different compaction styles.
bool DBHolder::ChangeCompactOptions() {
  if (option_config_ == kDefault) {
    option_config_ = kUniversalCompaction;
    Destroy(last_options_);
    auto options = CurrentOptions();
    options.create_if_missing = true;
    CHECK_OK(TryReopen(options));
    return true;
  } else if (option_config_ == kUniversalCompaction) {
    option_config_ = kUniversalCompactionMultiLevel;
    Destroy(last_options_);
    auto options = CurrentOptions();
    options.create_if_missing = true;
    CHECK_OK(TryReopen(options));
    return true;
  } else if (option_config_ == kUniversalCompactionMultiLevel) {
    option_config_ = kLevelSubcompactions;
    Destroy(last_options_);
    auto options = CurrentOptions();
    assert(options.max_subcompactions > 1);
    CHECK_OK(TryReopen(options));
    return true;
  } else if (option_config_ == kLevelSubcompactions) {
    option_config_ = kUniversalSubcompactions;
    Destroy(last_options_);
    auto options = CurrentOptions();
    assert(options.max_subcompactions > 1);
    CHECK_OK(TryReopen(options));
    return true;
  } else {
    return false;
  }
}

// Switch between different filter policy
// Jump from kDefault to kFilter to kFullFilter
bool DBHolder::ChangeFilterOptions() {
  if (option_config_ == kDefault) {
    option_config_ = kFilter;
  } else if (option_config_ == kFilter) {
    option_config_ = kFullFilterWithNewTableReaderForCompactions;
  } else {
    return false;
  }
  Destroy(last_options_);

  auto options = CurrentOptions();
  options.create_if_missing = true;
  CHECK_OK(TryReopen(options));
  return true;
}

// Return the current option configuration.
Options DBHolder::CurrentOptions(
    const anon::OptionsOverride& options_override) {
  Options options;
  options.write_buffer_size = 4090 * 4096;
  return CurrentOptions(options, options_override);
}

Options DBHolder::CurrentOptions(
    const Options& defaultOptions,
    const anon::OptionsOverride& options_override) {
  // this redundant copy is to minimize code change w/o having lint error.
  Options options = defaultOptions;
  XFUNC_TEST("", "dbtest_options", inplace_options1, GetXFTestOptions,
             reinterpret_cast<Options*>(&options),
             options_override.skip_policy);
  BlockBasedTableOptions table_options;
  bool set_block_based_table_factory = true;
  switch (option_config_) {
#ifndef ROCKSDB_LITE
    case kHashSkipList:
      options.prefix_extractor.reset(NewFixedPrefixTransform(1));
      options.memtable_factory.reset(NewHashSkipListRepFactory(16));
      break;
    case kPlainTableFirstBytePrefix:
      options.table_factory.reset(new PlainTableFactory());
      options.prefix_extractor.reset(NewFixedPrefixTransform(1));
      options.allow_mmap_reads = true;
      options.max_sequential_skip_in_iterations = 999999;
      set_block_based_table_factory = false;
      break;
    case kPlainTableCappedPrefix:
      options.table_factory.reset(new PlainTableFactory());
      options.prefix_extractor.reset(NewCappedPrefixTransform(8));
      options.allow_mmap_reads = true;
      options.max_sequential_skip_in_iterations = 999999;
      set_block_based_table_factory = false;
      break;
    case kPlainTableCappedPrefixNonMmap:
      options.table_factory.reset(new PlainTableFactory());
      options.prefix_extractor.reset(NewCappedPrefixTransform(8));
      options.allow_mmap_reads = false;
      options.max_sequential_skip_in_iterations = 999999;
      set_block_based_table_factory = false;
      break;
    case kPlainTableAllBytesPrefix:
      options.table_factory.reset(new PlainTableFactory());
      options.prefix_extractor.reset(NewNoopTransform());
      options.allow_mmap_reads = true;
      options.max_sequential_skip_in_iterations = 999999;
      set_block_based_table_factory = false;
      break;
    case kVectorRep:
      options.memtable_factory.reset(new VectorRepFactory(100));
      break;
    case kHashLinkList:
      options.prefix_extractor.reset(NewFixedPrefixTransform(1));
      options.memtable_factory.reset(
          NewHashLinkListRepFactory(4, 0, 3, true, 4));
      break;
#endif  // ROCKSDB_LITE
    case kMergePut:
      options.merge_operator = MergeOperators::CreatePutOperator();
      break;
    case kFilter:
      table_options.filter_policy.reset(NewBloomFilterPolicy(10, true));
      break;
    case kFullFilterWithNewTableReaderForCompactions:
      table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
      options.new_table_reader_for_compaction_inputs = true;
      options.compaction_readahead_size = 10 * 1024 * 1024;
      break;
    case kUncompressed:
      options.compression = kNoCompression;
      break;
    case kNumLevel_3:
      options.num_levels = 3;
      break;
    case kDBLogDir:
      options.db_log_dir = alternative_db_log_dir_;
      break;
    case kWalDirAndMmapReads:
      options.wal_dir = alternative_wal_dir_;
      // mmap reads should be orthogonal to WalDir setting, so we piggyback to
      // this option config to test mmap reads as well
      options.allow_mmap_reads = true;
      break;
    case kManifestFileSize:
      options.max_manifest_file_size = 50;  // 50 bytes
      // YugaByte change: the break statement was missing here in RocksDB before we made implicit
      // fallthrough an error.
      break;
    case kPerfOptions:
      options.soft_rate_limit = 2.0;
      options.delayed_write_rate = 8 * 1024 * 1024;
      // TODO(3.13) -- test more options
      break;
    case kDeletesFilterFirst:
      options.filter_deletes = true;
      break;
    case kUniversalCompaction:
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 1;
      break;
    case kUniversalCompactionMultiLevel:
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 8;
      break;
    case kCompressedBlockCache:
      options.allow_mmap_writes = true;
      table_options.block_cache_compressed = NewLRUCache(8 * 1024 * 1024);
      break;
    case kInfiniteMaxOpenFiles:
      options.max_open_files = -1;
      break;
    case kxxHashChecksum: {
      table_options.checksum = kxxHash;
      break;
    }
    case kFIFOCompaction: {
      options.compaction_style = kCompactionStyleFIFO;
      break;
    }
    case kBlockBasedTableWithPrefixHashIndex: {
      table_options.index_type = IndexType::kHashSearch;
      options.prefix_extractor.reset(NewFixedPrefixTransform(1));
      break;
    }
    case kBlockBasedTableWithWholeKeyHashIndex: {
      table_options.index_type = IndexType::kHashSearch;
      options.prefix_extractor.reset(NewNoopTransform());
      break;
    }
    case kBlockBasedTableWithIndexRestartInterval: {
      table_options.index_block_restart_interval = 8;
      break;
    }
    case kOptimizeFiltersForHits: {
      options.optimize_filters_for_hits = true;
      set_block_based_table_factory = true;
      break;
    }
    case kRowCache: {
      options.row_cache = NewLRUCache(1024 * 1024);
      break;
    }
    case kRecycleLogFiles: {
      options.recycle_log_file_num = 2;
      break;
    }
    case kLevelSubcompactions: {
      options.max_subcompactions = 4;
      break;
    }
    case kUniversalSubcompactions: {
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 8;
      options.max_subcompactions = 4;
      break;
    }
    case kConcurrentSkipList: {
      options.allow_concurrent_memtable_write = true;
      options.enable_write_thread_adaptive_yield = true;
      break;
    }
    case kBlockBasedTableWithThreeSharedPartsKeyDeltaEncoding: {
      table_options.use_delta_encoding = true;
      table_options.data_block_key_value_encoding_format =
          KeyValueEncodingFormat::kKeyDeltaEncodingThreeSharedParts;
      break;
    }

    default:
      break;
  }

  if (options_override.filter_policy) {
    table_options.filter_policy = options_override.filter_policy;
  }
  if (set_block_based_table_factory) {
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  }
  options.env = env_;
  options.checkpoint_env = env_->IsPlainText() ? env_ : Env::Default();
  options.create_if_missing = true;
  options.fail_if_options_file_error = true;
  return options;
}

void DBHolder::CreateColumnFamilies(const std::vector<std::string>& cfs,
                                      const Options& options) {
  ColumnFamilyOptions cf_opts(options);
  size_t cfi = handles_.size();
  handles_.resize(cfi + cfs.size());
  for (auto cf : cfs) {
    ASSERT_OK(db_->CreateColumnFamily(cf_opts, cf, &handles_[cfi++]));
  }
}

void DBHolder::CreateAndReopenWithCF(const std::vector<std::string>& cfs,
                                       const Options& options) {
  CreateColumnFamilies(cfs, options);
  std::vector<std::string> cfs_plus_default = cfs;
  cfs_plus_default.insert(cfs_plus_default.begin(), kDefaultColumnFamilyName);
  ReopenWithColumnFamilies(cfs_plus_default, options);
}

void DBHolder::ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                          const std::vector<Options>& options) {
  ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
}

void DBHolder::ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                          const Options& options) {
  ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
}

Status DBHolder::TryReopenWithColumnFamilies(
    const std::vector<std::string>& cfs, const std::vector<Options>& options) {
  Close();
  EXPECT_EQ(cfs.size(), options.size());
  std::vector<ColumnFamilyDescriptor> column_families;
  for (size_t i = 0; i < cfs.size(); ++i) {
    column_families.push_back(ColumnFamilyDescriptor(cfs[i], options[i]));
  }
  DBOptions db_opts = DBOptions(options[0]);
  return DB::Open(db_opts, dbname_, column_families, &handles_, &db_);
}

Status DBHolder::TryReopenWithColumnFamilies(
    const std::vector<std::string>& cfs, const Options& options) {
  Close();
  std::vector<Options> v_opts(cfs.size(), options);
  return TryReopenWithColumnFamilies(cfs, v_opts);
}

void DBHolder::Reopen(const Options& options) {
  ASSERT_OK(TryReopen(options));
}

void DBHolder::Close() {
  for (auto h : handles_) {
    delete h;
  }
  handles_.clear();
  delete db_;
  db_ = nullptr;
}

void DBHolder::DestroyAndReopen(const Options& options) {
  // Destroy using last options
  Destroy(last_options_);
  ASSERT_OK(TryReopen(options));
}

void DBHolder::Destroy(const Options& options) {
  Close();
  ASSERT_OK(DestroyDB(dbname_, options));
}

Status DBHolder::ReadOnlyReopen(const Options& options) {
  return DB::OpenForReadOnly(options, dbname_, &db_);
}

Status DBHolder::TryReopen(const Options& options) {
  Close();
  last_options_ = options;
  return DB::Open(options, dbname_, &db_);
}

Status DBHolder::Flush(int cf) {
  if (cf == 0) {
    return db_->Flush(FlushOptions());
  } else {
    return db_->Flush(FlushOptions(), handles_[cf]);
  }
}

Status DBHolder::Put(const Slice& k, const Slice& v, WriteOptions wo) {
  if (kMergePut == option_config_) {
    return db_->Merge(wo, k, v);
  } else {
    return db_->Put(wo, k, v);
  }
}

Status DBHolder::Put(int cf, const Slice& k, const Slice& v,
                       WriteOptions wo) {
  if (kMergePut == option_config_) {
    return db_->Merge(wo, handles_[cf], k, v);
  } else {
    return db_->Put(wo, handles_[cf], k, v);
  }
}

Status DBHolder::Delete(const std::string& k) {
  return db_->Delete(WriteOptions(), k);
}

Status DBHolder::Delete(int cf, const std::string& k) {
  return db_->Delete(WriteOptions(), handles_[cf], k);
}

Status DBHolder::SingleDelete(const std::string& k) {
  return db_->SingleDelete(WriteOptions(), k);
}

Status DBHolder::SingleDelete(int cf, const std::string& k) {
  return db_->SingleDelete(WriteOptions(), handles_[cf], k);
}

std::string DBHolder::Get(const std::string& k, const Snapshot* snapshot) {
  ReadOptions options;
  options.verify_checksums = true;
  options.snapshot = snapshot;
  options.query_id = kInMultiTouchId;
  std::string result;
  Status s = db_->Get(options, k, &result);
  if (s.IsNotFound()) {
    result = "NOT_FOUND";
  } else if (!s.ok()) {
    result = s.ToString();
  }
  return result;
}

std::string DBHolder::Get(int cf, const std::string& k,
                            const Snapshot* snapshot) {
  ReadOptions options;
  options.verify_checksums = true;
  options.snapshot = snapshot;
  std::string result;
  Status s = db_->Get(options, handles_[cf], k, &result);
  if (s.IsNotFound()) {
    result = "NOT_FOUND";
  } else if (!s.ok()) {
    result = s.ToString();
  }
  return result;
}

uint64_t DBHolder::GetNumSnapshots() {
  uint64_t int_num;
  EXPECT_TRUE(dbfull()->GetIntProperty("rocksdb.num-snapshots", &int_num));
  return int_num;
}

uint64_t DBHolder::GetTimeOldestSnapshots() {
  uint64_t int_num;
  EXPECT_TRUE(
      dbfull()->GetIntProperty("rocksdb.oldest-snapshot-time", &int_num));
  return int_num;
}

// Return a string that contains all key,value pairs in order,
// formatted like "(k1->v1)(k2->v2)".
std::string DBHolder::Contents(int cf) {
  std::vector<std::string> forward;
  std::string result;
  Iterator* iter = (cf == 0) ? db_->NewIterator(ReadOptions())
                             : db_->NewIterator(ReadOptions(), handles_[cf]);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    std::string s = IterStatus(iter);
    result.push_back('(');
    result.append(s);
    result.push_back(')');
    forward.push_back(s);
  }

  // Check reverse iteration results are the reverse of forward results
  unsigned int matched = 0;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    EXPECT_LT(matched, forward.size());
    EXPECT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
    matched++;
  }
  EXPECT_EQ(matched, forward.size());

  delete iter;
  return result;
}

std::string DBHolder::AllEntriesFor(const Slice& user_key, int cf) {
  Arena arena;
  ScopedArenaIterator iter;
  if (cf == 0) {
    iter.set(dbfull()->NewInternalIterator(&arena));
  } else {
    iter.set(dbfull()->NewInternalIterator(&arena, handles_[cf]));
  }
  InternalKey target(user_key, kMaxSequenceNumber, kTypeValue);
  iter->Seek(target.Encode());
  std::string result;
  if (!iter->status().ok()) {
    result = iter->status().ToString();
  } else {
    result = "[ ";
    bool first = true;
    while (iter->Valid()) {
      ParsedInternalKey ikey(Slice(), 0, kTypeValue);
      if (!ParseInternalKey(iter->key(), &ikey)) {
        result += "CORRUPTED";
      } else {
        if (!last_options_.comparator->Equal(ikey.user_key, user_key)) {
          break;
        }
        if (!first) {
          result += ", ";
        }
        first = false;
        switch (ikey.type) {
          case kTypeValue:
            result += iter->value().ToString();
            break;
          case kTypeMerge:
            // keep it the same as kTypeValue for testing kMergePut
            result += iter->value().ToString();
            break;
          case kTypeDeletion:
            result += "DEL";
            break;
          case kTypeSingleDeletion:
            result += "SDEL";
            break;
          default:
            assert(false);
            break;
        }
      }
      iter->Next();
    }
    if (!first) {
      result += " ";
    }
    result += "]";
  }
  return result;
}

#ifndef ROCKSDB_LITE
int DBHolder::NumSortedRuns(int cf) {
  ColumnFamilyMetaData cf_meta;
  if (cf == 0) {
    db_->GetColumnFamilyMetaData(&cf_meta);
  } else {
    db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
  }
  int num_sr = static_cast<int>(cf_meta.levels[0].files.size());
  for (size_t i = 1U; i < cf_meta.levels.size(); i++) {
    if (cf_meta.levels[i].files.size() > 0) {
      num_sr++;
    }
  }
  return num_sr;
}

uint64_t DBHolder::TotalSize(int cf) {
  ColumnFamilyMetaData cf_meta;
  if (cf == 0) {
    db_->GetColumnFamilyMetaData(&cf_meta);
  } else {
    db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
  }
  return cf_meta.size;
}

uint64_t DBHolder::SizeAtLevel(int level) {
  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  uint64_t sum = 0;
  for (const auto& m : metadata) {
    if (m.level == level) {
      sum += m.total_size;
    }
  }
  return sum;
}

size_t DBHolder::TotalLiveFiles(int cf) {
  ColumnFamilyMetaData cf_meta;
  if (cf == 0) {
    db_->GetColumnFamilyMetaData(&cf_meta);
  } else {
    db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
  }
  size_t num_files = 0;
  for (auto& level : cf_meta.levels) {
    num_files += level.files.size();
  }
  return num_files;
}

size_t DBHolder::CountLiveFiles() {
  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  return metadata.size();
}
#endif  // ROCKSDB_LITE

int DBHolder::NumTableFilesAtLevel(int level, int cf) {
  std::string property;
  if (cf == 0) {
    // default cfd
    EXPECT_TRUE(db_->GetProperty(
        "rocksdb.num-files-at-level" + NumberToString(level), &property));
  } else {
    EXPECT_TRUE(db_->GetProperty(
        handles_[cf], "rocksdb.num-files-at-level" + NumberToString(level),
        &property));
  }
  return atoi(property.c_str());
}

int DBHolder::TotalTableFiles(int cf, int levels) {
  if (levels == -1) {
    levels = CurrentOptions().num_levels;
  }
  int result = 0;
  for (int level = 0; level < levels; level++) {
    result += NumTableFilesAtLevel(level, cf);
  }
  return result;
}

// Return spread of files per level
std::string DBHolder::FilesPerLevel(int cf) {
  int num_levels =
      (cf == 0) ? db_->NumberLevels() : db_->NumberLevels(handles_[1]);
  std::string result;
  size_t last_non_zero_offset = 0;
  for (int level = 0; level < num_levels; level++) {
    int f = NumTableFilesAtLevel(level, cf);
    char buf[100];
    snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
    result += buf;
    if (f > 0) {
      last_non_zero_offset = result.size();
    }
  }
  result.resize(last_non_zero_offset);
  return result;
}

size_t DBHolder::CountFiles() {
  std::vector<std::string> files;
  env_->GetChildrenWarnNotOk(dbname_, &files);

  std::vector<std::string> logfiles;
  if (dbname_ != last_options_.wal_dir) {
    env_->GetChildrenWarnNotOk(last_options_.wal_dir, &logfiles);
  }

  return files.size() + logfiles.size();
}

uint64_t DBHolder::Size(const Slice& start, const Slice& limit, int cf) {
  Range r(start, limit);
  uint64_t size;
  if (cf == 0) {
    db_->GetApproximateSizes(&r, 1, &size);
  } else {
    db_->GetApproximateSizes(handles_[1], &r, 1, &size);
  }
  return size;
}

void DBHolder::Compact(int cf, const Slice& start, const Slice& limit,
                         uint32_t target_path_id) {
  CompactRangeOptions compact_options;
  compact_options.target_path_id = target_path_id;
  ASSERT_OK(db_->CompactRange(compact_options, handles_[cf], &start, &limit));
}

void DBHolder::Compact(int cf, const Slice& start, const Slice& limit) {
  ASSERT_OK(
      db_->CompactRange(CompactRangeOptions(), handles_[cf], &start, &limit));
}

void DBHolder::Compact(const Slice& start, const Slice& limit) {
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), &start, &limit));
}

// Do n memtable compactions, each of which produces an sstable
// covering the range [small,large].
void DBHolder::MakeTables(int n, const std::string& small,
                            const std::string& large, int cf) {
  for (int i = 0; i < n; i++) {
    ASSERT_OK(Put(cf, small, "begin"));
    ASSERT_OK(Put(cf, large, "end"));
    ASSERT_OK(Flush(cf));
    MoveFilesToLevel(n - i - 1, cf);
  }
}

// Prevent pushing of new sstables into deeper levels by adding
// tables that cover a specified range to all levels.
void DBHolder::FillLevels(const std::string& smallest,
                            const std::string& largest, int cf) {
  MakeTables(db_->NumberLevels(handles_[cf]), smallest, largest, cf);
}

void DBHolder::MoveFilesToLevel(int level, int cf) {
  for (int l = 0; l < level; ++l) {
    if (cf > 0) {
      CHECK_OK(dbfull()->TEST_CompactRange(l, nullptr, nullptr, handles_[cf]));
    } else {
      CHECK_OK(dbfull()->TEST_CompactRange(l, nullptr, nullptr));
    }
  }
}

void DBHolder::DumpFileCounts(const char* label) {
  fprintf(stderr, "---\n%s:\n", label);
  fprintf(stderr, "maxoverlap: %" PRIu64 "\n",
          dbfull()->TEST_MaxNextLevelOverlappingBytes());
  for (int level = 0; level < db_->NumberLevels(); level++) {
    int num = NumTableFilesAtLevel(level);
    if (num > 0) {
      fprintf(stderr, "  level %3d : %d files\n", level, num);
    }
  }
}

std::string DBHolder::DumpSSTableList() {
  std::string property;
  db_->GetProperty("rocksdb.sstables", &property);
  return property;
}

void DBHolder::GetSstFiles(std::string path,
                             std::vector<std::string>* files) {
  env_->GetChildrenWarnNotOk(path, files);

  files->erase(
      std::remove_if(files->begin(), files->end(), [](std::string name) {
        uint64_t number;
        FileType type;
        return !(ParseFileName(name, &number, &type) && type == kTableFile);
      }), files->end());
}

int DBHolder::GetSstFileCount(std::string path) {
  std::vector<std::string> files;
  GetSstFiles(path, &files);
  return static_cast<int>(files.size());
}

// this will generate non-overlapping files since it keeps increasing key_idx
void DBHolder::GenerateNewFile(int cf, Random* rnd, int* key_idx,
                                 bool nowait) {
  for (int i = 0; i < KNumKeysByGenerateNewFile; i++) {
    ASSERT_OK(Put(cf, Key(*key_idx), RandomString(rnd, (i == 99) ? 1 : 990)));
    (*key_idx)++;
  }
  if (!nowait) {
    CHECK_OK(dbfull()->TEST_WaitForFlushMemTable());
    CHECK_OK(dbfull()->TEST_WaitForCompact());
  }
}

// this will generate non-overlapping files since it keeps increasing key_idx
void DBHolder::GenerateNewFile(Random* rnd, int* key_idx, bool nowait) {
  for (int i = 0; i < KNumKeysByGenerateNewFile; i++) {
    ASSERT_OK(Put(Key(*key_idx), RandomString(rnd, (i == 99) ? 1 : 990)));
    (*key_idx)++;
  }
  if (!nowait) {
    CHECK_OK(dbfull()->TEST_WaitForFlushMemTable());
    CHECK_OK(dbfull()->TEST_WaitForCompact());
  }
}

const int DBHolder::kNumKeysByGenerateNewRandomFile = 51;

void DBHolder::GenerateNewRandomFile(Random* rnd, bool nowait) {
  for (int i = 0; i < kNumKeysByGenerateNewRandomFile; i++) {
    ASSERT_OK(Put("key" + RandomString(rnd, 7), RandomString(rnd, 2000)));
  }
  ASSERT_OK(Put("key" + RandomString(rnd, 7), RandomString(rnd, 200)));
  if (!nowait) {
    CHECK_OK(dbfull()->TEST_WaitForFlushMemTable());
    CHECK_OK(dbfull()->TEST_WaitForCompact());
  }
}

std::string DBHolder::IterStatus(Iterator* iter) {
  std::string result;
  if (iter->Valid()) {
    result = iter->key().ToString() + "->" + iter->value().ToString();
  } else {
    result = "(invalid)";
  }
  return result;
}

Options DBHolder::OptionsForLogIterTest() {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.WAL_ttl_seconds = 1000;
  return options;
}

std::string DBHolder::DummyString(size_t len, char c) {
  return std::string(len, c);
}

void DBHolder::VerifyIterLast(std::string expected_key, int cf) {
  Iterator* iter;
  ReadOptions ro;
  if (cf == 0) {
    iter = db_->NewIterator(ro);
  } else {
    iter = db_->NewIterator(ro, handles_[cf]);
  }
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), expected_key);
  delete iter;
}

// Used to test InplaceUpdate

// If previous value is nullptr or delta is > than previous value,
//   sets newValue with delta
// If previous value is not empty,
//   updates previous value with 'b' string of previous value size - 1.
UpdateStatus DBHolder::updateInPlaceSmallerSize(char* prevValue,
                                                  uint32_t* prevSize,
                                                  Slice delta,
                                                  std::string* newValue) {
  if (prevValue == nullptr) {
    *newValue = std::string(delta.size(), 'c');
    return UpdateStatus::UPDATED;
  } else {
    *prevSize = *prevSize - 1;
    std::string str_b = std::string(*prevSize, 'b');
    memcpy(prevValue, str_b.c_str(), str_b.size());
    return UpdateStatus::UPDATED_INPLACE;
  }
}

UpdateStatus DBHolder::updateInPlaceSmallerVarintSize(char* prevValue,
                                                        uint32_t* prevSize,
                                                        Slice delta,
                                                        std::string* newValue) {
  if (prevValue == nullptr) {
    *newValue = std::string(delta.size(), 'c');
    return UpdateStatus::UPDATED;
  } else {
    *prevSize = 1;
    std::string str_b = std::string(*prevSize, 'b');
    memcpy(prevValue, str_b.c_str(), str_b.size());
    return UpdateStatus::UPDATED_INPLACE;
  }
}

UpdateStatus DBHolder::updateInPlaceLargerSize(char* prevValue,
                                                 uint32_t* prevSize,
                                                 Slice delta,
                                                 std::string* newValue) {
  *newValue = std::string(delta.size(), 'c');
  return UpdateStatus::UPDATED;
}

UpdateStatus DBHolder::updateInPlaceNoAction(char* prevValue,
                                               uint32_t* prevSize, Slice delta,
                                               std::string* newValue) {
  return UpdateStatus::UPDATE_FAILED;
}

// Utility method to test InplaceUpdate
void DBHolder::validateNumberOfEntries(int numValues, int cf) {
  Arena arena;
  ScopedArenaIterator iter;
  if (cf != 0) {
    iter.set(dbfull()->NewInternalIterator(&arena, handles_[cf]));
  } else {
    iter.set(dbfull()->NewInternalIterator(&arena));
  }
  iter->SeekToFirst();
  ASSERT_EQ(iter->status().ok(), true);
  int seq = numValues;
  while (iter->Valid()) {
    ParsedInternalKey ikey;
    ikey.sequence = -1;
    ASSERT_EQ(ParseInternalKey(iter->key(), &ikey), true);

    // checks sequence number for updates
    ASSERT_EQ(ikey.sequence, (unsigned)seq--);
    iter->Next();
  }
  ASSERT_EQ(0, seq);
}

void DBHolder::CopyFile(const std::string& source,
                          const std::string& destination, uint64_t size) {
  const EnvOptions soptions;
  unique_ptr<SequentialFile> srcfile;
  ASSERT_OK(env_->NewSequentialFile(source, &srcfile, soptions));
  unique_ptr<WritableFile> destfile;
  ASSERT_OK(env_->NewWritableFile(destination, &destfile, soptions));

  if (size == 0) {
    // default argument means copy everything
    ASSERT_OK(env_->GetFileSize(source, &size));
  }

  uint8_t buffer[4096];
  Slice slice;
  while (size > 0) {
    uint64_t one = std::min(uint64_t(sizeof(buffer)), size);
    ASSERT_OK(srcfile->Read(one, &slice, buffer));
    ASSERT_OK(destfile->Append(slice));
    size -= slice.size();
  }
  ASSERT_OK(destfile->Close());
}

std::unordered_map<std::string, uint64_t> DBHolder::GetAllSSTFiles(
    uint64_t* total_size) {
  std::unordered_map<std::string, uint64_t> res;

  if (total_size) {
    *total_size = 0;
  }
  std::vector<std::string> files;
  env_->GetChildrenWarnNotOk(dbname_, &files);
  for (auto& file_name : files) {
    uint64_t number;
    FileType type;
    std::string file_path = dbname_ + "/" + file_name;
    if (ParseFileName(file_name, &number, &type) &&
        (type == kTableFile || type == kTableSBlockFile)) {
      uint64_t file_size = 0;
      CHECK_OK(env_->GetFileSize(file_path, &file_size));
      res[file_path] = file_size;
      if (total_size) {
        *total_size += file_size;
      }
    }
  }
  return res;
}

void ConfigureLoggingToGlog(Options* options, const std::string& log_prefix) {
  options->log_prefix = log_prefix;
  options->info_log_level = InfoLogLevel::INFO_LEVEL;
  options->info_log = std::make_shared<yb::YBRocksDBLogger>(options->log_prefix);
}

}  // namespace rocksdb
