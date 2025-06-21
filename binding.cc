#define NAPI_VERSION 8

#include <assert.h>
#include <napi-macros.h>
#include <node_api.h>

#include <rocksdb/cache.h>
#include <rocksdb/comparator.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "max_rev_operator.h"
#include "util.h"

class NullLogger : public rocksdb::Logger {
 public:
  using rocksdb::Logger::Logv;
  virtual void Logv(const char* format, va_list ap) override {}
  virtual size_t GetLogFileSize() const override { return 0; }
};

struct Database;
class Iterator;
class Updates;

struct ColumnFamily {
  rocksdb::ColumnFamilyHandle* handle;
  rocksdb::ColumnFamilyDescriptor descriptor;
};

struct Closable {
  virtual ~Closable() {}
  virtual rocksdb::Status Close() = 0;
};

struct Database final {
  Database(std::string location) : location(std::move(location)) {}
  ~Database() { assert(!db); }

  rocksdb::Status Close() {
    if (!db) {
      return rocksdb::Status::OK();
    }

    std::set<Closable*> closables;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closables = std::move(closables_);
    }

    for (auto closable : closables) {
      closable->Close();
    }

    db->FlushWAL(true);

    for (auto& [id, column] : columns) {
      db->DestroyColumnFamilyHandle(column.handle);
    }
    columns.clear();

    auto db2 = std::move(db);
    return db2->Close();
  }

  void Attach(Closable* closable) {
    std::lock_guard<std::mutex> lock(mutex_);

    closables_.insert(closable);
  }

  void Detach(Closable* closable) {
    std::lock_guard<std::mutex> lock(mutex_);

    closables_.erase(closable);
  }

  const std::string location;

  std::unique_ptr<rocksdb::DB> db;
  std::map<int32_t, ColumnFamily> columns;

 private:
  mutable std::mutex mutex_;
  std::set<Closable*> closables_;
};

enum BatchOp { Empty, Put, Delete, Merge, Data };

struct BatchEntry {
  BatchOp op = BatchOp::Empty;
  std::optional<std::string> key = std::nullopt;
  std::optional<std::string> val = std::nullopt;
  std::optional<ColumnFamily> column = std::nullopt;
};

struct BatchIterator : public rocksdb::WriteBatch::Handler {
  BatchIterator(Database* database,
                const bool keys,
                const bool values,
                const bool data,
                const rocksdb::ColumnFamilyHandle* column,
                const Encoding keyEncoding,
                const Encoding valueEncoding)
      : database_(database),
        keys_(keys),
        values_(values),
        data_(data),
        column_(column),
        keyEncoding_(keyEncoding),
        valueEncoding_(valueEncoding) {}

  napi_status Iterate(napi_env env, const rocksdb::WriteBatch& batch, napi_value* result) {
    cache_.reserve(batch.Count());

    ROCKS_STATUS_RETURN_NAPI(batch.Iterate(this));

    napi_value putStr;
    NAPI_STATUS_RETURN(napi_create_string_utf8(env, "put", NAPI_AUTO_LENGTH, &putStr));

    napi_value delStr;
    NAPI_STATUS_RETURN(napi_create_string_utf8(env, "del", NAPI_AUTO_LENGTH, &delStr));

    napi_value mergeStr;
    NAPI_STATUS_RETURN(napi_create_string_utf8(env, "merge", NAPI_AUTO_LENGTH, &mergeStr));

    napi_value dataStr;
    NAPI_STATUS_RETURN(napi_create_string_utf8(env, "data", NAPI_AUTO_LENGTH, &dataStr));

    napi_value nullVal;
    NAPI_STATUS_RETURN(napi_get_null(env, &nullVal));

    NAPI_STATUS_RETURN(napi_create_array_with_length(env, cache_.size() * 4, result));
    for (size_t n = 0; n < cache_.size(); ++n) {
      napi_value op;
      if (cache_[n].op == BatchOp::Put) {
        op = putStr;
      } else if (cache_[n].op == BatchOp::Delete) {
        op = delStr;
      } else if (cache_[n].op == BatchOp::Merge) {
        op = mergeStr;
      } else if (cache_[n].op == BatchOp::Data) {
        op = dataStr;
      } else {
        continue;
      }

      NAPI_STATUS_RETURN(napi_set_element(env, *result, n * 4 + 0, op));

      napi_value key;
      NAPI_STATUS_RETURN(Convert(env, cache_[n].key, keyEncoding_, key));
      NAPI_STATUS_RETURN(napi_set_element(env, *result, n * 4 + 1, key));

      napi_value val;
      NAPI_STATUS_RETURN(Convert(env, cache_[n].val, valueEncoding_, val));
      NAPI_STATUS_RETURN(napi_set_element(env, *result, n * 4 + 2, val));

      // TODO (fix)
      // napi_value column = cache_[n].column ? cache_[n].column->val : nullVal;
      NAPI_STATUS_RETURN(napi_set_element(env, *result, n * 4 + 3, nullVal));
    }

    cache_.clear();

    return napi_ok;
  }

  rocksdb::Status PutCF(uint32_t column_family_id, const rocksdb::Slice& key, const rocksdb::Slice& value) override {
    if (column_ && column_->GetID() != column_family_id) {
      return rocksdb::Status::OK();
    }

    BatchEntry entry = {BatchOp::Put};

    if (keys_) {
      entry.key = key.ToStringView();
    }

    if (values_) {
      entry.val = value.ToStringView();
    }

    // if (database_ && database_->columns.find(column_family_id) != database_->columns.end()) {
    //   entry.column = database_->columns[column_family_id];
    // }

    cache_.push_back(entry);

    return rocksdb::Status::OK();
  }

  rocksdb::Status DeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
    if (column_ && column_->GetID() != column_family_id) {
      return rocksdb::Status::OK();
    }

    BatchEntry entry = {BatchOp::Delete};

    if (keys_) {
      entry.key = key.ToStringView();
    }

    // if (database_ && database_->columns.find(column_family_id) != database_->columns.end()) {
    //   entry.column = database_->columns[column_family_id];
    // }

    cache_.push_back(entry);

    return rocksdb::Status::OK();
  }

  rocksdb::Status MergeCF(uint32_t column_family_id, const rocksdb::Slice& key, const rocksdb::Slice& value) override {
    if (column_ && column_->GetID() != column_family_id) {
      return rocksdb::Status::OK();
    }

    BatchEntry entry = {BatchOp::Merge};

    if (keys_) {
      entry.key = key.ToStringView();
    }

    if (values_) {
      entry.val = value.ToStringView();
    }

    // if (database_ && database_->columns.find(column_family_id) != database_->columns.end()) {
    //   entry.column = database_->columns[column_family_id];
    // }

    cache_.push_back(entry);

    return rocksdb::Status::OK();
  }

  void LogData(const rocksdb::Slice& data) override {
    if (!data_) {
      return;
    }

    BatchEntry entry = {BatchOp::Data};

    entry.val = data.ToStringView();

    cache_.push_back(entry);
  }

  bool Continue() override { return true; }

 private:
  Database* database_;
  const bool keys_;
  const bool values_;
  const bool data_;
  const rocksdb::ColumnFamilyHandle* column_;
  const Encoding keyEncoding_;
  const Encoding valueEncoding_;
  std::vector<BatchEntry> cache_;
};

struct BaseIterator : public Closable {
  BaseIterator(Database* database,
               rocksdb::ColumnFamilyHandle* column,
               const bool reverse,
               const std::optional<std::string>& lt,
               const std::optional<std::string>& lte,
               const std::optional<std::string>& gt,
               const std::optional<std::string>& gte,
               const int limit,
               rocksdb::ReadOptions readOptions = {})
      : database_(database), column_(column), reverse_(reverse), limit_(limit) {
    if (lte) {
      upper_bound_ = rocksdb::PinnableSlice();
      *upper_bound_->GetSelf() = std::move(*lte) + '\0';
      upper_bound_->PinSelf();
    } else if (lt) {
      upper_bound_ = rocksdb::PinnableSlice();
      *upper_bound_->GetSelf() = std::move(*lt);
      upper_bound_->PinSelf();
    }

    if (gte) {
      lower_bound_ = rocksdb::PinnableSlice();
      *lower_bound_->GetSelf() = std::move(*gte);
      lower_bound_->PinSelf();
    } else if (gt) {
      lower_bound_ = rocksdb::PinnableSlice();
      *lower_bound_->GetSelf() = std::move(*gt) + '\0';
      lower_bound_->PinSelf();
    }

    if (upper_bound_) {
      readOptions.iterate_upper_bound = &*upper_bound_;
    }

    if (lower_bound_) {
      readOptions.iterate_lower_bound = &*lower_bound_;
    }

    iterator_.reset(database_->db->NewIterator(readOptions, column_));

    if (reverse_) {
      iterator_->SeekToLast();
    } else {
      iterator_->SeekToFirst();
    }

    database_->Attach(this);
  }

  virtual ~BaseIterator() {
    if (iterator_) {
      database_->Detach(this);
    }
  }

  virtual void Seek(const rocksdb::Slice& target) {
    assert(iterator_);

    if ((upper_bound_ && target.compare(*upper_bound_) >= 0) || (lower_bound_ && target.compare(*lower_bound_) < 0)) {
      // TODO (fix): Why is this required? Seek should handle it?
      // https://github.com/facebook/rocksdb/issues/9904
      iterator_->SeekToLast();
      if (iterator_->Valid()) {
        iterator_->Next();
      }
    } else if (reverse_) {
      iterator_->SeekForPrev(target);
    } else {
      iterator_->Seek(target);
    }
  }

  virtual rocksdb::Status Close() override {
    if (iterator_) {
      lower_bound_.reset();
      upper_bound_.reset();
      iterator_.reset();
      database_->Detach(this);
    }
    return rocksdb::Status::OK();
  }

  bool Valid() const {
    assert(iterator_);
    return iterator_->Valid();
  }

  bool Increment() {
    assert(iterator_);
    return limit_ < 0 || ++count_ <= limit_;
  }

  void Next() {
    assert(iterator_);

    if (reverse_)
      iterator_->Prev();
    else
      iterator_->Next();
  }

  rocksdb::Slice CurrentKey() const {
    assert(iterator_);
    return iterator_->key();
  }

  rocksdb::Slice CurrentValue() const {
    assert(iterator_);
    return iterator_->value();
  }

  rocksdb::Status Status() const {
    assert(iterator_);
    return iterator_->status();
  }

  Database* database_;
  rocksdb::ColumnFamilyHandle* column_;

 private:
  int count_ = 0;
  std::optional<rocksdb::PinnableSlice> lower_bound_;
  std::optional<rocksdb::PinnableSlice> upper_bound_;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  const bool reverse_;
  const int limit_;
};

class Iterator final : public BaseIterator {
  const bool keys_;
  const bool values_;
  const size_t highWaterMarkBytes_;
  bool first_ = true;
  const Encoding keyEncoding_;
  const Encoding valueEncoding_;

 public:
  Iterator(Database* database,
           rocksdb::ColumnFamilyHandle* column,
           const bool reverse,
           const bool keys,
           const bool values,
           const int limit,
           const std::optional<std::string>& lt,
           const std::optional<std::string>& lte,
           const std::optional<std::string>& gt,
           const std::optional<std::string>& gte,
           const size_t highWaterMarkBytes,
           Encoding keyEncoding = Encoding::Invalid,
           Encoding valueEncoding = Encoding::Invalid,
           rocksdb::ReadOptions readOptions = {})
      : BaseIterator(database, column, reverse, lt, lte, gt, gte, limit, readOptions),
        keys_(keys),
        values_(values),
        highWaterMarkBytes_(highWaterMarkBytes),
        keyEncoding_(keyEncoding),
        valueEncoding_(valueEncoding) {
  }

  void Seek(const rocksdb::Slice& target) override {
    first_ = true;
    return BaseIterator::Seek(target);
  }

  static std::unique_ptr<Iterator> create(napi_env env, napi_value db, napi_value options) {
    Database* database;
    NAPI_STATUS_THROWS(napi_get_value_external(env, db, reinterpret_cast<void**>(&database)));

    bool reverse = false;
    NAPI_STATUS_THROWS(GetProperty(env, options, "reverse", reverse));

    bool keys = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "keys", keys));

    bool values = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "values", values));

    int32_t limit = -1;
    NAPI_STATUS_THROWS(GetProperty(env, options, "limit", limit));

    int32_t highWaterMarkBytes = std::numeric_limits<int32_t>::max();
    NAPI_STATUS_THROWS(GetProperty(env, options, "highWaterMarkBytes", highWaterMarkBytes));

    std::optional<std::string> lt;
    NAPI_STATUS_THROWS(GetProperty(env, options, "lt", lt));

    std::optional<std::string> lte;
    NAPI_STATUS_THROWS(GetProperty(env, options, "lte", lte));

    std::optional<std::string> gt;
    NAPI_STATUS_THROWS(GetProperty(env, options, "gt", gt));

    std::optional<std::string> gte;
    NAPI_STATUS_THROWS(GetProperty(env, options, "gte", gte));

    rocksdb::ColumnFamilyHandle* column = database->db->DefaultColumnFamily();
    NAPI_STATUS_THROWS(GetProperty(env, options, "column", column));

    Encoding keyEncoding;
    NAPI_STATUS_THROWS(GetProperty(env, options, "keyEncoding", keyEncoding));

    Encoding valueEncoding;
    NAPI_STATUS_THROWS(GetProperty(env, options, "valueEncoding", valueEncoding));

    rocksdb::ReadOptions readOptions;

    readOptions.background_purge_on_iterator_cleanup = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "backgroundPurgeOnIteratorCleanup", readOptions.background_purge_on_iterator_cleanup));

    readOptions.tailing = false;
    NAPI_STATUS_THROWS(GetProperty(env, options, "tailing", readOptions.tailing));

    readOptions.fill_cache = false;
    NAPI_STATUS_THROWS(GetProperty(env, options, "fillCache", readOptions.fill_cache));

    readOptions.async_io = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "asyncIO", readOptions.async_io));

    readOptions.adaptive_readahead = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "adaptiveReadahead", readOptions.adaptive_readahead));

    readOptions.readahead_size = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "readaheadSize", readOptions.readahead_size));

    readOptions.auto_readahead_size = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "autoReadaheadSize", readOptions.auto_readahead_size));

    readOptions.ignore_range_deletions = false;
    NAPI_STATUS_THROWS(GetProperty(env, options, "ignoreRangeDeletions", readOptions.ignore_range_deletions));

    if (!values) {
      readOptions.allow_unprepared_value = true;
    }

    // uint32_t timeout = 0;
    // NAPI_STATUS_THROWS(GetProperty(env, options, "timeout", timeout));

    // readOptions.deadline = timeout
    //   ? std::chrono::microseconds(database->db->GetEnv()->NowMicros() + timeout * 1000)
    //   : std::chrono::microseconds::zero();

    return std::make_unique<Iterator>(database, column, reverse, keys, values, limit, lt, lte, gt, gte,
                                      highWaterMarkBytes, keyEncoding, valueEncoding, readOptions);
  }

  napi_value nextv(napi_env env, uint32_t count, uint32_t timeout, napi_value callback) {
    struct State {
      std::vector<rocksdb::PinnableSlice> keys;
      std::vector<rocksdb::PinnableSlice> values;
      size_t count = 0;
      bool finished = false;
    };

    runAsync<State>("iterator.nextv", env, callback,
        [=](auto& state) {
          state.keys.reserve(count);
          state.values.reserve(count);

          const auto deadline = timeout
            ? database_->db->GetEnv()->NowMicros() + timeout * 1000
            : 0;

          size_t bytesRead = 0;
          for (int n = 0; n < count; n++) {
            if (!first_) {
              Next();
            } else {
              first_ = false;
            }

            ROCKS_STATUS_RETURN(Status());

            if (!Valid() || !Increment()) {
              state.finished = true;
              break;
            }

            if (keys_ && values_) {
              rocksdb::PinnableSlice k;
              k.PinSelf(CurrentKey());
              state.keys.push_back(std::move(k));

              rocksdb::PinnableSlice v;
              v.PinSelf(CurrentValue());
              state.values.push_back(std::move(v));
            } else if (keys_) {
              rocksdb::PinnableSlice k;
              k.PinSelf(CurrentKey());
              state.keys.push_back(std::move(k));
            } else if (values_) {
              rocksdb::PinnableSlice v;
              v.PinSelf(CurrentValue());
              state.values.push_back(std::move(v));
            } else {
              assert(false);
            }
            state.count += 1;

            bytesRead += CurrentKey().size() + CurrentValue().size();
            if (bytesRead > highWaterMarkBytes_) {
              break;
            }

            if (deadline > 0 && database_->db->GetEnv()->NowMicros() > deadline) {
              break;
            }
          }

          return rocksdb::Status::OK();
        },
        [=](auto& state, auto env, auto& argv) {
          argv.resize(2);

          napi_value finished;
          NAPI_STATUS_RETURN(napi_get_boolean(env, state.finished, &finished));

          napi_value rows;
          NAPI_STATUS_RETURN(napi_create_array(env, &rows));

          for (size_t n = 0; n < state.count; n++) {
            napi_value key;
            napi_value val;

            if (keys_ && values_) {
              NAPI_STATUS_RETURN(Convert(env, std::move(state.keys[n]), keyEncoding_, key));
              NAPI_STATUS_RETURN(Convert(env, std::move(state.values[n]), valueEncoding_, val));
            } else if (keys_) {
              NAPI_STATUS_RETURN(Convert(env, std::move(state.keys[n]), keyEncoding_, key));
              NAPI_STATUS_RETURN(napi_get_undefined(env, &val));
            } else if (values_) {
              NAPI_STATUS_RETURN(napi_get_undefined(env, &key));
              NAPI_STATUS_RETURN(Convert(env, std::move(state.values[n]), valueEncoding_, val));
            } else {
              assert(false);
            }

            NAPI_STATUS_RETURN(napi_set_element(env, rows, n * 2 + 0, key));
            NAPI_STATUS_RETURN(napi_set_element(env, rows, n * 2 + 1, val));
          }

          NAPI_STATUS_RETURN(napi_create_object(env, &argv[1]));
          NAPI_STATUS_RETURN(napi_set_named_property(env, argv[1], "rows", rows));
          NAPI_STATUS_RETURN(napi_set_named_property(env, argv[1], "finished", finished));

          return napi_ok;
        });

    return 0;
  }

  napi_value nextv(napi_env env, uint32_t count, uint32_t timeout = 0) {
    napi_value finished;
    NAPI_STATUS_THROWS(napi_get_boolean(env, false, &finished));

    napi_value rows;
    NAPI_STATUS_THROWS(napi_create_array(env, &rows));

    const auto deadline = timeout
      ? database_->db->GetEnv()->NowMicros() + timeout * 1000
      : 0;

    size_t idx = 0;
    size_t bytesRead = 0;
    for (int n = 0; n < count; n++) {
      if (!first_) {
        Next();
      } else {
        first_ = false;
      }

      ROCKS_STATUS_THROWS_NAPI(Status());

      if (!Valid() || !Increment()) {
        NAPI_STATUS_THROWS(napi_get_boolean(env, true, &finished));
        break;
      }

      napi_value key;
      napi_value val;

      if (keys_ && values_) {
        const auto k = CurrentKey();
        const auto v = CurrentValue();
        NAPI_STATUS_THROWS(Convert(env, &k, keyEncoding_, key));
        NAPI_STATUS_THROWS(Convert(env, &v, valueEncoding_, val));
      } else if (keys_) {
        const auto k = CurrentKey();
        NAPI_STATUS_THROWS(Convert(env, &k, keyEncoding_, key));
        NAPI_STATUS_THROWS(napi_get_undefined(env, &val));
      } else if (values_) {
        const auto v = CurrentValue();
        NAPI_STATUS_THROWS(napi_get_undefined(env, &key));
        NAPI_STATUS_THROWS(Convert(env, &v, valueEncoding_, val));
      } else {
        assert(false);
      }

      NAPI_STATUS_THROWS(napi_set_element(env, rows, idx++, key));
      NAPI_STATUS_THROWS(napi_set_element(env, rows, idx++, val));

      bytesRead += CurrentKey().size() + CurrentValue().size();
      if (bytesRead > highWaterMarkBytes_) {
        break;
      }

      if (deadline > 0 && database_->db->GetEnv()->NowMicros() > deadline) {
        break;
      }
    }

    napi_value ret;
    NAPI_STATUS_THROWS(napi_create_object(env, &ret));
    NAPI_STATUS_THROWS(napi_set_named_property(env, ret, "rows", rows));
    NAPI_STATUS_THROWS(napi_set_named_property(env, ret, "finished", finished));
    return ret;
  }
};

/**
 * Hook for when the environment exits. This hook will be called after
 * already-scheduled napi_async_work items have finished, which gives us
 * the guarantee that no db operations will be in-flight at this time.
 */
static void env_cleanup_hook(void* data) {
  auto database = reinterpret_cast<Database*>(data);

  // Do everything that db_close() does but synchronously. We're expecting that GC
  // did not (yet) collect the database because that would be a user mistake (not
  // closing their db) made during the lifetime of the environment. That's different
  // from an environment being torn down (like the main process or a worker thread)
  // where it's our responsibility to clean up. Note also, the following code must
  // be a safe noop if called before db_open() or after db_close().
  if (database) {
    database->Close();
  }
}

static void FinalizeDatabase(napi_env env, void* data, void* hint) {
  auto database = reinterpret_cast<Database*>(data);
  if (database) {
    napi_remove_env_cleanup_hook(env, env_cleanup_hook, database);
    database->Close();
  }
}

NAPI_METHOD(db_init) {
  NAPI_ARGV(1);

  Database* database = nullptr;

  napi_valuetype type;
  NAPI_STATUS_THROWS(napi_typeof(env, argv[0], &type));

  napi_value result;

  if (type == napi_string) {
    std::string location;
    size_t length = 0;
    NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length));
    location.resize(length, '\0');
    NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, argv[0], &location[0], length + 1, &length));

    database = new Database(location);
    napi_add_env_cleanup_hook(env, env_cleanup_hook, database);
    NAPI_STATUS_THROWS(napi_create_external(env, database, FinalizeDatabase, nullptr, &result));
  } else if (type == napi_bigint) {
    int64_t value;
    bool lossless;
    NAPI_STATUS_THROWS(napi_get_value_bigint_int64(env, argv[0], &value, &lossless));

    database = reinterpret_cast<Database*>(value);
    NAPI_STATUS_THROWS(napi_create_external(env, database, nullptr, nullptr, &result));

    // We should have an env_cleanup_hook for closing iterators...
  } else {
    NAPI_STATUS_THROWS(napi_invalid_arg);
  }

  return result;
}

NAPI_METHOD(db_get_handle) {
  NAPI_ARGV(1);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_bigint_int64(env, reinterpret_cast<intptr_t>(database), &result));

  return result;
}

NAPI_METHOD(db_get_location) {
  NAPI_ARGV(1);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  napi_value result;
  NAPI_STATUS_THROWS(Convert(env, &database->location, Encoding::String, result));

  return result;
}

NAPI_METHOD(db_query) {
  NAPI_ARGV(2);

  try {
    return Iterator::create(env, argv[0], argv[1])->nextv(env, std::numeric_limits<uint32_t>::max());
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

template <typename T, typename U>
napi_status InitOptions(napi_env env, T& columnOptions, const U& options) {
  rocksdb::ConfigOptions configOptions;

  uint64_t memtable_memory_budget = 256 * 1024 * 1024;
  NAPI_STATUS_RETURN(GetProperty(env, options, "memtableMemoryBudget", memtable_memory_budget));

  std::string compaction;
  NAPI_STATUS_RETURN(GetProperty(env, options, "compaction", compaction));
  if (compaction == "") {
    // Do nothing...
  } else if (compaction == "universal") {
    columnOptions.write_buffer_size = memtable_memory_budget / 4;
    // merge two memtables when flushing to L0
    columnOptions.min_write_buffer_number_to_merge = 2;
    // this means we'll use 50% extra memory in the worst case, but will reduce
    // write stalls.
    columnOptions.max_write_buffer_number = 6;
    // universal style compaction
    columnOptions.compaction_style = rocksdb::kCompactionStyleUniversal;
    columnOptions.compaction_options_universal.compression_size_percent = 80;
  } else if (compaction == "level") {
    columnOptions.write_buffer_size = static_cast<size_t>(memtable_memory_budget / 4);
    // merge two memtables when flushing to L0
    columnOptions.min_write_buffer_number_to_merge = 2;
    // this means we'll use 50% extra memory in the worst case, but will reduce
    // write stalls.
    columnOptions.max_write_buffer_number = 6;
    // start flushing L0->L1 as soon as possible. each file on level0 is
    // (memtable_memory_budget / 2). This will flush level 0 when it's bigger than
    // memtable_memory_budget.
    columnOptions.level0_file_num_compaction_trigger = 2;
    // doesn't really matter much, but we don't want to create too many files
    columnOptions.target_file_size_base = memtable_memory_budget / 8;
    // make Level1 size equal to Level0 size, so that L0->L1 compactions are fast
    columnOptions.max_bytes_for_level_base = memtable_memory_budget;

    // level style compaction
    columnOptions.compaction_style = rocksdb::kCompactionStyleLevel;

    // only compress levels >= 2
    columnOptions.compression_per_level.resize(columnOptions.num_levels);
    for (int i = 0; i < columnOptions.num_levels; ++i) {
      if (i < 2) {
        columnOptions.compression_per_level[i] = rocksdb::kNoCompression;
      } else {
        columnOptions.compression_per_level[i] = rocksdb::kZSTD;
      }
    }
  } else {
    return napi_invalid_arg;
  }

  bool compression = true;
  NAPI_STATUS_RETURN(GetProperty(env, options, "compression", compression));

  if (compression) {
    columnOptions.compression = rocksdb::kZSTD;
    columnOptions.compression_opts.max_dict_bytes = 16 * 1024;
    columnOptions.compression_opts.zstd_max_train_bytes = 16 * 1024 * 100;
    // TODO (perf): compression_opts.parallel_threads
  }

  std::string prefixExtractor;
  NAPI_STATUS_RETURN(GetProperty(env, options, "prefixExtractor", prefixExtractor));
  if (prefixExtractor == "") {
    // Do nothing...
  } else {
    ROCKS_STATUS_RETURN_NAPI(
        rocksdb::SliceTransform::CreateFromString(configOptions, prefixExtractor, &columnOptions.prefix_extractor));
  }

  std::string comparator;
  NAPI_STATUS_RETURN(GetProperty(env, options, "comparator", comparator));
  if (comparator == "") {
    // Do nothing...
  } else {
    ROCKS_STATUS_RETURN_NAPI(
        rocksdb::Comparator::CreateFromString(configOptions, comparator, &columnOptions.comparator));
  }

  std::string mergeOperator;
  NAPI_STATUS_RETURN(GetProperty(env, options, "mergeOperator", mergeOperator));
  if (mergeOperator == "") {
    // Do nothing...
  } else if (mergeOperator == "maxRev") {
    columnOptions.merge_operator = std::make_shared<MaxRevOperator>();
  } else {
    ROCKS_STATUS_RETURN_NAPI(
        rocksdb::MergeOperator::CreateFromString(configOptions, mergeOperator, &columnOptions.merge_operator));
  }

  std::string compactionPriority;
  NAPI_STATUS_RETURN(GetProperty(env, options, "compactionPriority", compactionPriority));
  if (compactionPriority == "") {
    // Do nothing...
  } else if (compactionPriority == "byCompensatedSize") {
    columnOptions.compaction_pri = rocksdb::kByCompensatedSize;
  } else if (compactionPriority == "oldestLargestSeqFirst") {
    columnOptions.compaction_pri = rocksdb::kOldestLargestSeqFirst;
  } else if (compactionPriority == "smallestSeqFirst") {
    columnOptions.compaction_pri = rocksdb::kOldestSmallestSeqFirst;
  } else if (compactionPriority == "overlappingRatio") {
    columnOptions.compaction_pri = rocksdb::kMinOverlappingRatio;
  } else if (compactionPriority == "roundRobin") {
    columnOptions.compaction_pri = rocksdb::kRoundRobin;
  } else {
    return napi_invalid_arg;
  }

  NAPI_STATUS_RETURN(GetProperty(env, options, "optimizeFiltersForHits", columnOptions.optimize_filters_for_hits));
  NAPI_STATUS_RETURN(GetProperty(env, options, "periodicCompactionSeconds", columnOptions.periodic_compaction_seconds));

  // Compat
  NAPI_STATUS_RETURN(GetProperty(env, options, "enableBlobFiles", columnOptions.enable_blob_files));
  NAPI_STATUS_RETURN(GetProperty(env, options, "minBlobSize", columnOptions.min_blob_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "enableBlobGarbageCollection", columnOptions.enable_blob_garbage_collection));

  NAPI_STATUS_RETURN(GetProperty(env, options, "blobFiles", columnOptions.enable_blob_files));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobMinSize", columnOptions.min_blob_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobGarbageCollection", columnOptions.enable_blob_garbage_collection));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobFileSize", columnOptions.blob_file_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobGarbageCollectionAgeCutoff", columnOptions.blob_garbage_collection_age_cutoff));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobGarbageCollectionForceThreshold", columnOptions.blob_garbage_collection_force_threshold));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobCompactionReadaheadSize", columnOptions.blob_compaction_readahead_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobFileStartingLevel", columnOptions.blob_file_starting_level));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blobCompression", columnOptions.blob_compression_type));

  rocksdb::BlockBasedTableOptions tableOptions;
  tableOptions.decouple_partitioned_filters = true;

  std::shared_ptr<rocksdb::Cache> cache;
  {
    uint32_t cacheSize = 8 << 20;
    double compressedRatio = 0.0;

    NAPI_STATUS_RETURN(GetProperty(env, options, "cacheSize", cacheSize));
    NAPI_STATUS_RETURN(GetProperty(env, options, "cacheCompressedRatio", compressedRatio));

    if (cacheSize == 0) {
      cache = nullptr;
    } else if (compressedRatio > 0.0) {
      rocksdb::TieredCacheOptions options;
      options.cache_type = rocksdb::PrimaryCacheType::kCacheTypeHCC;
      options.total_capacity = cacheSize;
      options.compressed_secondary_ratio = compressedRatio;
      cache = rocksdb::NewTieredCache(options);
    } else {
      cache = rocksdb::HyperClockCacheOptions(cacheSize, 0).MakeSharedCache();
    }
  }

  {
    uint32_t cacheSize = -1;
    double compressedRatio = 0.0;

    NAPI_STATUS_RETURN(GetProperty(env, options, "cachePrepopulate", tableOptions.prepopulate_block_cache));
    NAPI_STATUS_RETURN(GetProperty(env, options, "prepopulateBlockCache", tableOptions.prepopulate_block_cache));

    NAPI_STATUS_RETURN(GetProperty(env, options, "blockCacheSize", cacheSize));
    NAPI_STATUS_RETURN(GetProperty(env, options, "blockCacheCompressedRatio", compressedRatio));
    NAPI_STATUS_RETURN(GetProperty(env, options, "blockCachePrepopulate", tableOptions.prepopulate_block_cache));

    if (cacheSize == -1) {
      if (cache) {
        tableOptions.block_cache = cache;
      } else {
        tableOptions.no_block_cache = true;
      }
    } else if (compressedRatio > 0.0) {
      rocksdb::TieredCacheOptions options;
      options.cache_type = rocksdb::PrimaryCacheType::kCacheTypeHCC;
      options.total_capacity = cacheSize;
      options.compressed_secondary_ratio = compressedRatio;
      tableOptions.block_cache = rocksdb::NewTieredCache(options);
    } else {
      tableOptions.block_cache = rocksdb::HyperClockCacheOptions(cacheSize, 0).MakeSharedCache();
    }
  }

  {
    uint32_t cacheSize = -1;
    double compressedRatio = 0.0;

    NAPI_STATUS_RETURN(GetProperty(env, options, "cachePrepopulate", columnOptions.prepopulate_blob_cache));
    NAPI_STATUS_RETURN(GetProperty(env, options, "prepopulateBlobCache", columnOptions.prepopulate_blob_cache));

    NAPI_STATUS_RETURN(GetProperty(env, options, "blobCacheSize", cacheSize));
    NAPI_STATUS_RETURN(GetProperty(env, options, "blobCacheCompressedRatio", compressedRatio));
    NAPI_STATUS_RETURN(GetProperty(env, options, "blobCachePrepopulate", columnOptions.prepopulate_blob_cache));

    if (cacheSize == -1) {
      columnOptions.blob_cache = cache;
    } else if (cacheSize == 0) {
      columnOptions.blob_cache = nullptr;
    } else  if (compressedRatio > 0.0) {
      rocksdb::TieredCacheOptions options;
      options.total_capacity = cacheSize;
      options.compressed_secondary_ratio = compressedRatio;
      options.comp_cache_opts.compression_type = rocksdb::CompressionType::kZSTD;
      columnOptions.blob_cache = rocksdb::NewTieredCache(options);
    } else {
      columnOptions.blob_cache = rocksdb::HyperClockCacheOptions(cacheSize, 0).MakeSharedCache();
    }
  }

  std::string optimize = "";
  NAPI_STATUS_RETURN(GetProperty(env, options, "optimize", optimize));

  if (optimize == "") {
    tableOptions.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  } else if (optimize == "point-lookup") {
    tableOptions.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
    tableOptions.data_block_hash_table_util_ratio = 0.75;
    tableOptions.filter_policy.reset(rocksdb::NewRibbonFilterPolicy(10, 2));

    columnOptions.memtable_prefix_bloom_size_ratio = 0.02;
    columnOptions.memtable_whole_key_filtering = true;
  } else if (optimize == "range-lookup") {
    // TODO?
  } else {
    return napi_invalid_arg;
  }

  std::string indexType;
  NAPI_STATUS_RETURN(GetProperty(env, options, "indexType", indexType));
  if (indexType == "") {
    // Do nothing...
  } else if (indexType == "binarySearch") {
    tableOptions.index_type = rocksdb::BlockBasedTableOptions::kBinarySearch;
  } else if (indexType == "hashSearch") {
    tableOptions.index_type = rocksdb::BlockBasedTableOptions::kHashSearch;
  } else if (indexType == "twoLevelIndexSearch") {
    tableOptions.index_type = rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch;
  } else if (indexType == "binarySearchWithFirstKey") {
    tableOptions.index_type = rocksdb::BlockBasedTableOptions::kBinarySearchWithFirstKey;
  } else {
    return napi_invalid_arg;
  }

  std::string dataBlockIndexType;
  NAPI_STATUS_RETURN(GetProperty(env, options, "dataBlockIndexType", dataBlockIndexType));
  if (dataBlockIndexType == "") {
    // Do nothing...
  } else if (dataBlockIndexType == "dataBlockBinarySearch") {
    tableOptions.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinarySearch;
  } else if (dataBlockIndexType == "dataBlockBinaryAndHash") {
    tableOptions.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
  } else {
    return napi_invalid_arg;
  }

  std::string filterPolicy;
  NAPI_STATUS_RETURN(GetProperty(env, options, "filterPolicy", filterPolicy));
  if (filterPolicy != "") {
    ROCKS_STATUS_RETURN_NAPI(
        rocksdb::FilterPolicy::CreateFromString(configOptions, filterPolicy, &tableOptions.filter_policy));
  }

  std::string indexShortening;
  NAPI_STATUS_RETURN(GetProperty(env, options, "indexShortening", indexShortening));
  if (indexShortening == "") {
    // Do nothing..
  } else if (indexShortening == "noShortening") {
    tableOptions.index_shortening = rocksdb::BlockBasedTableOptions::IndexShorteningMode::kNoShortening;
  } else if (indexShortening == "shortenSeparators") {
    tableOptions.index_shortening = rocksdb::BlockBasedTableOptions::IndexShorteningMode::kShortenSeparators;
  } else if (indexShortening == "shortenSeparatorsAndSuccessor") {
    tableOptions.index_shortening = rocksdb::BlockBasedTableOptions::IndexShorteningMode::kShortenSeparatorsAndSuccessor;
  } else {
    return napi_invalid_arg;
  }

  NAPI_STATUS_RETURN(GetProperty(env, options, "dataBlockHashTableUtilRatio", tableOptions.data_block_hash_table_util_ratio));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blockSize", tableOptions.block_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blockRestartInterval", tableOptions.block_restart_interval));
  NAPI_STATUS_RETURN(GetProperty(env, options, "blockAlign", tableOptions.block_align));
  NAPI_STATUS_RETURN(GetProperty(env, options, "cacheIndexAndFilterBlocks", tableOptions.cache_index_and_filter_blocks));
  NAPI_STATUS_RETURN(GetProperty(env, options, "cacheIndexAndFilterBlocksWithHighPriority", tableOptions.cache_index_and_filter_blocks_with_high_priority));
  NAPI_STATUS_RETURN(GetProperty(env, options, "decouplePartitionedFilters", tableOptions.block_restart_interval));
  NAPI_STATUS_RETURN(GetProperty(env, options, "optimizeFiltersForMemory", tableOptions.optimize_filters_for_memory));
  NAPI_STATUS_RETURN(GetProperty(env, options, "maxAutoReadaheadSize", tableOptions.max_auto_readahead_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "initialAutoReadaheadSize", tableOptions.initial_auto_readahead_size));
  NAPI_STATUS_RETURN(GetProperty(env, options, "numFileReadsForAutoReadahead", tableOptions.num_file_reads_for_auto_readahead));

  columnOptions.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tableOptions));

  return napi_ok;
}

NAPI_METHOD(db_get_identity) {
  NAPI_ARGV(1);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  std::string identity;
  ROCKS_STATUS_THROWS_NAPI(database->db->GetDbIdentity(identity));

  napi_value result;
  NAPI_STATUS_THROWS(Convert(env, &identity, Encoding::String, result));

  return result;
}

NAPI_METHOD(db_open) {
  NAPI_ARGV(3);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  if (database->db) {
    napi_value columns;
    NAPI_STATUS_THROWS(napi_create_object(env, &columns));
    for (auto& [id, column] : database->columns) {
      napi_value val;
      NAPI_STATUS_THROWS(napi_create_external(env, column.handle, nullptr, nullptr, &val));
      NAPI_STATUS_THROWS(napi_set_named_property(env, columns, column.descriptor.name.c_str(), val));
    }
    return columns;
  } else {
    rocksdb::Options dbOptions;

    const auto options = argv[1];

    int parallelism = std::max<int>(1, std::thread::hardware_concurrency() / 2);
    NAPI_STATUS_THROWS(GetProperty(env, options, "parallelism", parallelism));
    dbOptions.IncreaseParallelism(parallelism);

    NAPI_STATUS_THROWS(GetProperty(env, options, "walDir", dbOptions.wal_dir));

    uint32_t walTTL = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walTTL", walTTL));
    dbOptions.WAL_ttl_seconds = walTTL / 1e3;

    uint32_t walSizeLimit = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walSizeLimit", walSizeLimit));
    dbOptions.WAL_size_limit_MB = walSizeLimit / 1e6;

    NAPI_STATUS_THROWS(GetProperty(env, options, "maxTotalWalSize", dbOptions.max_total_wal_size));

    bool walCompression = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walCompression", walCompression));
    dbOptions.wal_compression =
        walCompression ? rocksdb::CompressionType::kZSTD : rocksdb::CompressionType::kNoCompression;

    dbOptions.avoid_unnecessary_blocking_io = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "avoidUnnecessaryBlockingIO", dbOptions.avoid_unnecessary_blocking_io));

    dbOptions.create_missing_column_families = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "createMissingColumnFamilies", dbOptions.create_missing_column_families));

    NAPI_STATUS_THROWS(GetProperty(env, options, "writeDbIdToManifest", dbOptions.write_dbid_to_manifest));

    NAPI_STATUS_THROWS(GetProperty(env, options, "failIfOptionsFileError", dbOptions.fail_if_options_file_error));

    NAPI_STATUS_THROWS(GetProperty(env, options, "adviseRandomOnOpen", dbOptions.advise_random_on_open));

    NAPI_STATUS_THROWS(GetProperty(env, options, "bytesPerSync", dbOptions.bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "walBytesPerSync", dbOptions.wal_bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "strictBytesPerSync", dbOptions.strict_bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "delayedWriteRate", dbOptions.delayed_write_rate));

    NAPI_STATUS_THROWS(GetProperty(env, options, "createIfMissing", dbOptions.create_if_missing));

    NAPI_STATUS_THROWS(GetProperty(env, options, "errorIfExists", dbOptions.error_if_exists));

    NAPI_STATUS_THROWS(GetProperty(env, options, "pipelinedWrite", dbOptions.enable_pipelined_write));

    NAPI_STATUS_THROWS(GetProperty(env, options, "dailyOffpeakTime", dbOptions.daily_offpeak_time_utc));

    NAPI_STATUS_THROWS(GetProperty(env, options, "unorderedWrite", dbOptions.unordered_write));

    NAPI_STATUS_THROWS(GetProperty(env, options, "allowMmapReads", dbOptions.allow_mmap_reads));

    NAPI_STATUS_THROWS(GetProperty(env, options, "allowMmapWrites", dbOptions.allow_mmap_writes));

    NAPI_STATUS_THROWS(GetProperty(env, options, "memTableHugePageSize", dbOptions.memtable_huge_page_size));

    NAPI_STATUS_THROWS(GetProperty(env, options, "useDirectIOReads", dbOptions.use_direct_reads));

    NAPI_STATUS_THROWS(GetProperty(env, options, "useDirectIOForFlushAndCompaction", dbOptions.use_direct_io_for_flush_and_compaction));

    NAPI_STATUS_THROWS(GetProperty(env, options, "compactionReadaheadSize", dbOptions.compaction_readahead_size));

    // TODO (feat): dbOptions.listeners

    std::string infoLogLevel;
    NAPI_STATUS_THROWS(GetProperty(env, options, "infoLogLevel", infoLogLevel));
    if (infoLogLevel.size() > 0) {
      rocksdb::InfoLogLevel lvl = {};

      if (infoLogLevel == "debug")
        lvl = rocksdb::InfoLogLevel::DEBUG_LEVEL;
      else if (infoLogLevel == "info")
        lvl = rocksdb::InfoLogLevel::INFO_LEVEL;
      else if (infoLogLevel == "warn")
        lvl = rocksdb::InfoLogLevel::WARN_LEVEL;
      else if (infoLogLevel == "error")
        lvl = rocksdb::InfoLogLevel::ERROR_LEVEL;
      else if (infoLogLevel == "fatal")
        lvl = rocksdb::InfoLogLevel::FATAL_LEVEL;
      else if (infoLogLevel == "header")
        lvl = rocksdb::InfoLogLevel::HEADER_LEVEL;
      else
        napi_throw_error(env, nullptr, "invalid log level");

      dbOptions.info_log_level = lvl;
    } else {
      // In some places RocksDB checks this option to see if it should prepare
      // debug information (ahead of logging), so set it to the highest level.
      dbOptions.info_log_level = rocksdb::InfoLogLevel::HEADER_LEVEL;
      dbOptions.info_log.reset(new NullLogger());
    }

    NAPI_STATUS_THROWS(InitOptions(env, dbOptions, options));

    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;

    bool hasColumns;
    NAPI_STATUS_THROWS(napi_has_named_property(env, options, "columns", &hasColumns));

    if (hasColumns) {
      napi_value columns;
      NAPI_STATUS_THROWS(napi_get_named_property(env, options, "columns", &columns));

      napi_value keys;
      NAPI_STATUS_THROWS(napi_get_property_names(env, columns, &keys));

      uint32_t len;
      NAPI_STATUS_THROWS(napi_get_array_length(env, keys, &len));

      descriptors.resize(len);
      for (uint32_t n = 0; n < len; ++n) {
        napi_value key;
        NAPI_STATUS_THROWS(napi_get_element(env, keys, n, &key));

        napi_value column;
        NAPI_STATUS_THROWS(napi_get_property(env, columns, key, &column));

        NAPI_STATUS_THROWS(InitOptions(env, descriptors[n].options, column));

        NAPI_STATUS_THROWS(GetValue(env, key, descriptors[n].name));
      }
    }

    auto callback = argv[2];

    runAsync<std::vector<rocksdb::ColumnFamilyHandle*>>(
        "leveldown.open", env, callback,
        [=](auto& handles) {
          assert(!database->db);

          rocksdb::DB* db = nullptr;

          const auto status = descriptors.empty()
                                  ? rocksdb::DB::Open(dbOptions, database->location, &db)
                                  : rocksdb::DB::Open(dbOptions, database->location, descriptors, &handles, &db);

          database->db.reset(db);

          return status;
        },
        [=](auto& handles, auto env, auto& argv) {
          argv.resize(2);

          NAPI_STATUS_RETURN(napi_create_object(env, &argv[1]));

          for (size_t n = 0; n < handles.size(); ++n) {
            ColumnFamily column;
            column.handle = handles[n];
            column.descriptor = descriptors[n];
            database->columns[column.handle->GetID()] = column;
          }

          napi_value columns = argv[1];
          for (auto& [id, column] : database->columns) {
            napi_value val;
            NAPI_STATUS_RETURN(napi_create_external(env, column.handle, nullptr, nullptr, &val));
            NAPI_STATUS_RETURN(napi_set_named_property(env, columns, column.descriptor.name.c_str(), val));
          }

          return napi_ok;
        });
  }

  return 0;
}

napi_value noop_callback(napi_env env, napi_callback_info info) {
  return 0;
}

NAPI_METHOD(db_close) {
  NAPI_ARGV(2);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  auto callback = argv[1];

  struct State {};
  runAsync<State>(
      "leveldown.close", env, callback, [=](auto& state) { return database->Close(); },
      [](auto& state, auto env, auto& argv) { return napi_ok; });

  return 0;
}

NAPI_METHOD(db_get_many_sync) {
  NAPI_ARGV(3);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  uint32_t count;
  NAPI_STATUS_THROWS(napi_get_array_length(env, argv[1], &count));

  rocksdb::ColumnFamilyHandle* column = database->db->DefaultColumnFamily();
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "column", column));

  Encoding valueEncoding = Encoding::Buffer;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "valueEncoding", valueEncoding));

  uint32_t timeout = 0;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "timeout", timeout));

  bool unsafe = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "unsafe", unsafe));

  std::vector<rocksdb::Slice> keys;
  keys.resize(count);
  std::vector<rocksdb::Status> statuses;
  statuses.resize(count);
  std::vector<rocksdb::PinnableSlice> values;
  values.resize(count);

  for (uint32_t n = 0; n < count; n++) {
    napi_value element;
    NAPI_STATUS_THROWS(napi_get_element(env, argv[1], n, &element));
    NAPI_STATUS_THROWS(GetValue(env, element, keys[n]));
  }

  rocksdb::ReadOptions readOptions;
  readOptions.deadline = timeout
    ? std::chrono::microseconds(database->db->GetEnv()->NowMicros() + timeout * 1000)
    : std::chrono::microseconds::zero();

  readOptions.fill_cache = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "fillCache", readOptions.fill_cache));

  readOptions.async_io = true;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "asyncIO", readOptions.async_io));

  readOptions.optimize_multiget_for_io = true;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "optimizeMultigetForIO", readOptions.optimize_multiget_for_io));

  readOptions.value_size_soft_limit = std::numeric_limits<int32_t>::max();
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "highWaterMarkBytes", readOptions.value_size_soft_limit));

  database->db->MultiGet(readOptions, column, count, keys.data(), values.data(), statuses.data());

  napi_value rows;
  NAPI_STATUS_THROWS(napi_create_array_with_length(env, count, &rows));

  for (uint32_t n = 0; n < count; n++) {
    napi_value row;
    if (statuses[n].IsNotFound()) {
      NAPI_STATUS_THROWS(napi_get_undefined(env, &row));
    } else if (statuses[n].IsAborted() || statuses[n].IsTimedOut()) {
      NAPI_STATUS_THROWS(napi_get_null(env, &row));
    } else {
      ROCKS_STATUS_THROWS_NAPI(statuses[n]);
      if (unsafe) {
        NAPI_STATUS_THROWS(ConvertUnsafe(env, std::move(values[n]), valueEncoding, row));
      } else {
        NAPI_STATUS_THROWS(Convert(env, std::move(values[n]), valueEncoding, row));
      }
    }
    NAPI_STATUS_THROWS(napi_set_element(env, rows, n, row));
  }

  return rows;
}

NAPI_METHOD(db_get_many) {
  NAPI_ARGV(4);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  uint32_t count;
  NAPI_STATUS_THROWS(napi_get_array_length(env, argv[1], &count));

  rocksdb::ColumnFamilyHandle* column = database->db->DefaultColumnFamily();
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "column", column));

  Encoding valueEncoding = Encoding::Buffer;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "valueEncoding", valueEncoding));

  bool unsafe = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "unsafe", unsafe));

  auto callback = argv[3];

  struct State {
    rocksdb::ReadOptions readOptions;
    std::vector<rocksdb::Status> statuses;
    std::vector<rocksdb::PinnableSlice> values;
    std::vector<rocksdb::PinnableSlice> keys;
  } state;

  state.keys.resize(count);

  for (uint32_t n = 0; n < count; n++) {
    napi_value element;
    NAPI_STATUS_THROWS(napi_get_element(env, argv[1], n, &element));
    NAPI_STATUS_THROWS(GetValue(env, element, state.keys[n]));
  }

  state.readOptions.fill_cache = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "fillCache", state.readOptions.fill_cache));

  state.readOptions.async_io = true;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "asyncIO", state.readOptions.async_io));

  state.readOptions.optimize_multiget_for_io = true;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "optimizeMultigetForIO", state.readOptions.optimize_multiget_for_io));

  state.readOptions.value_size_soft_limit = std::numeric_limits<int32_t>::max();
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "highWaterMarkBytes", state.readOptions.value_size_soft_limit));

  runAsync(std::move(state),
      "leveldown.get_many", env, callback,
      [=](auto& state) {
        std::vector<rocksdb::Slice> keys;
        keys.reserve(count);
        for (uint32_t n = 0; n < count; n++) {
          keys.emplace_back(state.keys[n]);
        }

        state.statuses.resize(count);
        state.values.resize(count);

        database->db->MultiGet(state.readOptions, column, count, keys.data(), state.values.data(), state.statuses.data());

        return rocksdb::Status::OK();
      },
      [=](auto& state, auto env, auto& argv) {
        argv.resize(2);

        NAPI_STATUS_RETURN(napi_create_array_with_length(env, count, &argv[1]));

        for (auto n = 0; n < count; n++) {
          napi_value row;
          if (state.statuses[n].IsNotFound()) {
            NAPI_STATUS_RETURN(napi_get_undefined(env, &row));
          } else if (state.statuses[n].IsAborted()) {
            NAPI_STATUS_RETURN(napi_get_null(env, &row));
          } else {
            ROCKS_STATUS_RETURN_NAPI(state.statuses[n]);
            if (unsafe) {
              NAPI_STATUS_RETURN(ConvertUnsafe(env, std::move(state.values[n]), valueEncoding, row));
            } else {
              NAPI_STATUS_RETURN(Convert(env, std::move(state.values[n]), valueEncoding, row));
            }
          }
          NAPI_STATUS_RETURN(napi_set_element(env, argv[1], n, row));
        }

        return napi_ok;
      });

  return 0;
}

NAPI_METHOD(db_clear) {
  NAPI_ARGV(2);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  const auto options = argv[1];

  bool reverse = false;
  NAPI_STATUS_THROWS(GetProperty(env, options, "reverse", reverse));

  int32_t limit = -1;
  NAPI_STATUS_THROWS(GetProperty(env, options, "limit", limit));

  rocksdb::ColumnFamilyHandle* column = database->db->DefaultColumnFamily();
  NAPI_STATUS_THROWS(GetProperty(env, options, "column", column));

  std::optional<std::string> lt;
  NAPI_STATUS_THROWS(GetProperty(env, options, "lt", lt));

  std::optional<std::string> lte;
  NAPI_STATUS_THROWS(GetProperty(env, options, "lte", lte));

  std::optional<std::string> gt;
  NAPI_STATUS_THROWS(GetProperty(env, options, "gt", gt));

  std::optional<std::string> gte;
  NAPI_STATUS_THROWS(GetProperty(env, options, "gte", gte));

  if (limit == -1) {
    rocksdb::PinnableSlice begin;
    if (gte) {
      *begin.GetSelf() = std::move(*gte);
    } else if (gt) {
      *begin.GetSelf() = std::move(*gt) + '\0';
    }
    begin.PinSelf();

    rocksdb::PinnableSlice end;
    if (lte) {
      *end.GetSelf() = std::move(*lte) + '\0';
    } else if (lt) {
      *end.GetSelf() = std::move(*lt);
    } else {
      // HACK: Assume no key that starts with 0xFF is larger than 1MiB.
      end.GetSelf()->resize(1e6);
      memset(end.GetSelf()->data(), 255, end.GetSelf()->size());
    }
    end.PinSelf();

    if (begin.compare(end) < 0) {
      rocksdb::WriteOptions writeOptions;
      ROCKS_STATUS_THROWS_NAPI(database->db->DeleteRange(writeOptions, column, begin, end));
    }

    return 0;
  } else {
    // TODO (fix): Error handling.
    // TODO (fix): This should be async...

    BaseIterator it(database, column, reverse, lt, lte, gt, gte, limit);

    rocksdb::WriteBatch batch;
    rocksdb::WriteOptions writeOptions;
    rocksdb::Status status;

    while (true) {
      size_t bytesRead = 0;

      while (bytesRead <= 16 * 1024 && it.Valid() && it.Increment()) {
        const auto key = it.CurrentKey();
        batch.Delete(column, key);
        bytesRead += key.size();
        it.Next();
      }

      status = it.Status();
      if (!status.ok() || bytesRead == 0) {
        break;
      }

      status = database->db->Write(writeOptions, &batch);
      if (!status.ok()) {
        break;
      }

      batch.Clear();
    }

    it.Close();

    if (!status.ok()) {
      ROCKS_STATUS_THROWS_NAPI(status);
    }

    return 0;
  }
}

NAPI_METHOD(db_get_property) {
  NAPI_ARGV(2);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  rocksdb::PinnableSlice property;
  NAPI_STATUS_THROWS(GetValue(env, argv[1], property));

  std::string value;
  database->db->GetProperty(property, &value);

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_string_utf8(env, value.data(), value.size(), &result));

  return result;
}

NAPI_METHOD(db_get_latest_sequence) {
  NAPI_ARGV(1);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  const auto seq = database->db->GetLatestSequenceNumber();

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_int64(env, seq, &result));

  return result;
}

NAPI_METHOD(iterator_init) {
  NAPI_ARGV(2);

  napi_value result;
  try {
    auto iterator = Iterator::create(env, argv[0], argv[1]);

    NAPI_STATUS_THROWS(napi_create_external(env, iterator.get(), Finalize<Iterator>, iterator.get(), &result));
    iterator.release();
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }

  return result;
}

NAPI_METHOD(iterator_seek) {
  NAPI_ARGV(2);

  try {
    Iterator* iterator;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&iterator)));

    rocksdb::PinnableSlice target;
    NAPI_STATUS_THROWS(GetValue(env, argv[1], target));

    iterator->Seek(target);
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }

  return 0;
}

NAPI_METHOD(iterator_close) {
  NAPI_ARGV(1);

  try {
    Iterator* iterator;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&iterator)));

    ROCKS_STATUS_THROWS_NAPI(iterator->Close());
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }

  return 0;
}

NAPI_METHOD(iterator_nextv) {
  NAPI_ARGV(4);

  try {
    Iterator* iterator;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&iterator)));

    uint32_t count = 1024;
    NAPI_STATUS_THROWS(GetValue(env, argv[1], count));

    uint32_t timeout = 0;
    NAPI_STATUS_THROWS(GetProperty(env, argv[2], "timeout", timeout));

    return iterator->nextv(env, count, timeout, argv[3]);
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

NAPI_METHOD(iterator_nextv_sync) {
  NAPI_ARGV(3);

  try {
    Iterator* iterator;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&iterator)));

    uint32_t count = 1024;
    NAPI_STATUS_THROWS(GetValue(env, argv[1], count));

    uint32_t timeout = 0;
    NAPI_STATUS_THROWS(GetProperty(env, argv[2], "timeout", timeout));

    return iterator->nextv(env, count, timeout);
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

NAPI_METHOD(batch_init) {
  auto batch = std::make_unique<rocksdb::WriteBatch>();

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_external(env, batch.get(), Finalize<rocksdb::WriteBatch>, batch.get(), &result));
  batch.release();

  return result;
}

NAPI_METHOD(batch_put) {
  NAPI_ARGV(4);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  rocksdb::Slice key;
  NAPI_STATUS_THROWS(GetValue(env, argv[1], key));

  rocksdb::Slice val;
  NAPI_STATUS_THROWS(GetValue(env, argv[2], val));

  rocksdb::ColumnFamilyHandle* column = nullptr;
  NAPI_STATUS_THROWS(GetProperty(env, argv[3], "column", column));

  if (column) {
    ROCKS_STATUS_THROWS_NAPI(batch->Put(column, key, val));
  } else {
    ROCKS_STATUS_THROWS_NAPI(batch->Put(key, val));
  }

  return 0;
}

NAPI_METHOD(batch_put_log_data) {
  NAPI_ARGV(3);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  rocksdb::Slice blob;
  NAPI_STATUS_THROWS(GetValue(env, argv[1], blob));

  ROCKS_STATUS_THROWS_NAPI(batch->PutLogData(blob));

  return 0;
}

NAPI_METHOD(batch_del) {
  NAPI_ARGV(3);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  rocksdb::Slice key;
  NAPI_STATUS_THROWS(GetValue(env, argv[1], key));

  rocksdb::ColumnFamilyHandle* column = nullptr;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "column", column));

  if (column) {
    ROCKS_STATUS_THROWS_NAPI(batch->Delete(column, key));
  } else {
    ROCKS_STATUS_THROWS_NAPI(batch->Delete(key));
  }

  return 0;
}

NAPI_METHOD(batch_merge) {
  NAPI_ARGV(4);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  rocksdb::Slice key;
  NAPI_STATUS_THROWS(GetValue(env, argv[1], key));

  rocksdb::Slice val;
  NAPI_STATUS_THROWS(GetValue(env, argv[2], val));

  rocksdb::ColumnFamilyHandle* column = nullptr;
  NAPI_STATUS_THROWS(GetProperty(env, argv[3], "column", column));

  if (column) {
    ROCKS_STATUS_THROWS_NAPI(batch->Merge(column, key, val));
  } else {
    ROCKS_STATUS_THROWS_NAPI(batch->Merge(key, val));
  }

  return 0;
}

NAPI_METHOD(batch_clear) {
  NAPI_ARGV(1);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  batch->Clear();

  return 0;
}

NAPI_METHOD(batch_write) {
  NAPI_ARGV(3);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[1], reinterpret_cast<void**>(&batch)));

  bool sync = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "sync", sync));

  bool lowPriority = false;
  NAPI_STATUS_THROWS(GetProperty(env, argv[2], "lowPriority", lowPriority));

  rocksdb::WriteOptions writeOptions;
  writeOptions.sync = sync;
  writeOptions.low_pri = lowPriority;
  ROCKS_STATUS_THROWS_NAPI(database->db->Write(writeOptions, batch));

  return 0;
}

NAPI_METHOD(batch_count) {
  NAPI_ARGV(1);

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&batch)));

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_int64(env, batch->Count(), &result));

  return result;
}

NAPI_METHOD(batch_iterate) {
  NAPI_ARGV(3);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  rocksdb::WriteBatch* batch;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[1], reinterpret_cast<void**>(&batch)));

  const auto options = argv[2];

  bool keys = true;
  NAPI_STATUS_THROWS(GetProperty(env, options, "keys", keys));

  bool values = true;
  NAPI_STATUS_THROWS(GetProperty(env, options, "values", values));

  bool data = true;
  NAPI_STATUS_THROWS(GetProperty(env, options, "data", data));

  Encoding keyEncoding = Encoding::String;
  NAPI_STATUS_THROWS(GetProperty(env, options, "keyEncoding", keyEncoding));

  Encoding valueEncoding = Encoding::String;
  NAPI_STATUS_THROWS(GetProperty(env, options, "valueEncoding", valueEncoding));

  rocksdb::ColumnFamilyHandle* column = nullptr;
  NAPI_STATUS_THROWS(GetProperty(env, options, "column", column));

  BatchIterator iterator(nullptr, keys, values, data, column, keyEncoding, valueEncoding);

  napi_value result;
  NAPI_STATUS_THROWS(iterator.Iterate(env, *batch, &result));

  return result;
}

struct Updates : public BatchIterator, public Closable {
  Updates(Database* database,
          const int64_t since,
          const bool keys,
          const bool values,
          const bool data,
          const rocksdb::ColumnFamilyHandle* column,
          const Encoding keyEncoding,
          const Encoding valueEncoding)
      : BatchIterator(database, keys, values, data, column, keyEncoding, valueEncoding),
        database_(database),
        start_(since) {
    database_->Attach(this);
  }

  virtual ~Updates() {
    if (iterator_) {
      database_->Detach(this);
    }
  }

  rocksdb::Status Close() override {
    if (iterator_) {
      iterator_.reset();
      database_->Detach(this);
    }
    return rocksdb::Status::OK();
  }

  Database* database_;
  int64_t start_;
  std::unique_ptr<rocksdb::TransactionLogIterator> iterator_;

 private:
  napi_ref ref_ = nullptr;
};

NAPI_METHOD(updates_init) {
  NAPI_ARGV(2);

  try {
    Database* database;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

    const auto options = argv[1];

    int64_t since = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "since", since));

    bool keys = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "keys", keys));

    bool values = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "values", values));

    bool data = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "data", data));

    Encoding keyEncoding = Encoding::String;
    NAPI_STATUS_THROWS(GetProperty(env, options, "keyEncoding", keyEncoding));

    Encoding valueEncoding = Encoding::String;
    NAPI_STATUS_THROWS(GetProperty(env, options, "valueEncoding", valueEncoding));

    rocksdb::ColumnFamilyHandle* column = nullptr;
    NAPI_STATUS_THROWS(GetProperty(env, options, "column", column));

    napi_value result;
    auto updates =
      std::unique_ptr<Updates>(new Updates(database, since, keys, values, data, column, keyEncoding, valueEncoding));

    NAPI_STATUS_THROWS(napi_create_external(env, updates.get(), Finalize<Updates>, updates.get(), &result));
    updates.release();

    return result;
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

NAPI_METHOD(updates_next) {
  NAPI_ARGV(2);

  try {
    Updates* updates;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&updates)));

    if (!updates->iterator_) {
      rocksdb::TransactionLogIterator::ReadOptions options;
      ROCKS_STATUS_THROWS_NAPI(updates->database_->db->GetUpdatesSince(updates->start_, &updates->iterator_, options));
    }

    ROCKS_STATUS_THROWS_NAPI(updates->iterator_->status());

    if (!updates->iterator_->Valid()) {
      return 0;
    }

    auto batchResult = updates->iterator_->GetBatch();

    napi_value rows;
    napi_value sequence;

    NAPI_STATUS_THROWS(updates->Iterate(env, *batchResult.writeBatchPtr, &rows));
    NAPI_STATUS_THROWS(napi_create_int64(env, batchResult.sequence, &sequence));

    ROCKS_STATUS_THROWS_NAPI(updates->iterator_->status());
    updates->iterator_->Next();

    napi_value ret;
    NAPI_STATUS_THROWS(napi_create_object(env, &ret));
    NAPI_STATUS_THROWS(napi_set_named_property(env, ret, "rows", rows));
    NAPI_STATUS_THROWS(napi_set_named_property(env, ret, "seq", sequence));
    return ret;
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

NAPI_METHOD(updates_close) {
  NAPI_ARGV(1);

  try {
    Updates* updates;
    NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&updates)));

    ROCKS_STATUS_THROWS_NAPI(updates->Close());
    return 0;
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

NAPI_METHOD(db_open_for_read_only) {
  NAPI_ARGV(3);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  if (database->db) {
    napi_value columns;
    NAPI_STATUS_THROWS(napi_create_object(env, &columns));
    for (auto& [id, column] : database->columns) {
      napi_value val;
      NAPI_STATUS_THROWS(napi_create_external(env, column.handle, nullptr, nullptr, &val));
      NAPI_STATUS_THROWS(napi_set_named_property(env, columns, column.descriptor.name.c_str(), val));
    }
    return columns;
  } else {
    rocksdb::Options dbOptions;

    const auto options = argv[1];

    int parallelism = std::max<int>(1, std::thread::hardware_concurrency() / 2);
    NAPI_STATUS_THROWS(GetProperty(env, options, "parallelism", parallelism));
    dbOptions.IncreaseParallelism(parallelism);

    NAPI_STATUS_THROWS(GetProperty(env, options, "walDir", dbOptions.wal_dir));

    uint32_t walTTL = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walTTL", walTTL));
    dbOptions.WAL_ttl_seconds = walTTL / 1e3;

    uint32_t walSizeLimit = 0;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walSizeLimit", walSizeLimit));
    dbOptions.WAL_size_limit_MB = walSizeLimit / 1e6;

    NAPI_STATUS_THROWS(GetProperty(env, options, "maxTotalWalSize", dbOptions.max_total_wal_size));

    bool walCompression = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "walCompression", walCompression));
    dbOptions.wal_compression =
        walCompression ? rocksdb::CompressionType::kZSTD : rocksdb::CompressionType::kNoCompression;

    dbOptions.avoid_unnecessary_blocking_io = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "avoidUnnecessaryBlockingIO", dbOptions.avoid_unnecessary_blocking_io));

    dbOptions.create_missing_column_families = true;
    NAPI_STATUS_THROWS(GetProperty(env, options, "createMissingColumnFamilies", dbOptions.create_missing_column_families));

    NAPI_STATUS_THROWS(GetProperty(env, options, "writeDbIdToManifest", dbOptions.write_dbid_to_manifest));

    NAPI_STATUS_THROWS(GetProperty(env, options, "failIfOptionsFileError", dbOptions.fail_if_options_file_error));

    NAPI_STATUS_THROWS(GetProperty(env, options, "adviseRandomOnOpen", dbOptions.advise_random_on_open));

    NAPI_STATUS_THROWS(GetProperty(env, options, "bytesPerSync", dbOptions.bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "walBytesPerSync", dbOptions.wal_bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "strictBytesPerSync", dbOptions.strict_bytes_per_sync));

    NAPI_STATUS_THROWS(GetProperty(env, options, "delayedWriteRate", dbOptions.delayed_write_rate));

    NAPI_STATUS_THROWS(GetProperty(env, options, "createIfMissing", dbOptions.create_if_missing));

    NAPI_STATUS_THROWS(GetProperty(env, options, "errorIfExists", dbOptions.error_if_exists));

    NAPI_STATUS_THROWS(GetProperty(env, options, "pipelinedWrite", dbOptions.enable_pipelined_write));

    NAPI_STATUS_THROWS(GetProperty(env, options, "dailyOffpeakTime", dbOptions.daily_offpeak_time_utc));

    NAPI_STATUS_THROWS(GetProperty(env, options, "unorderedWrite", dbOptions.unordered_write));

    NAPI_STATUS_THROWS(GetProperty(env, options, "allowMmapReads", dbOptions.allow_mmap_reads));

    NAPI_STATUS_THROWS(GetProperty(env, options, "allowMmapWrites", dbOptions.allow_mmap_writes));

    NAPI_STATUS_THROWS(GetProperty(env, options, "memTableHugePageSize", dbOptions.memtable_huge_page_size));

    NAPI_STATUS_THROWS(GetProperty(env, options, "useDirectIOReads", dbOptions.use_direct_reads));

    NAPI_STATUS_THROWS(GetProperty(env, options, "useDirectIOForFlushAndCompaction", dbOptions.use_direct_io_for_flush_and_compaction));

    NAPI_STATUS_THROWS(GetProperty(env, options, "compactionReadaheadSize", dbOptions.compaction_readahead_size));

    // TODO (feat): dbOptions.listeners

    std::string infoLogLevel;
    NAPI_STATUS_THROWS(GetProperty(env, options, "infoLogLevel", infoLogLevel));
    if (infoLogLevel.size() > 0) {
      rocksdb::InfoLogLevel lvl = {};

      if (infoLogLevel == "debug")
        lvl = rocksdb::InfoLogLevel::DEBUG_LEVEL;
      else if (infoLogLevel == "info")
        lvl = rocksdb::InfoLogLevel::INFO_LEVEL;
      else if (infoLogLevel == "warn")
        lvl = rocksdb::InfoLogLevel::WARN_LEVEL;
      else if (infoLogLevel == "error")
        lvl = rocksdb::InfoLogLevel::ERROR_LEVEL;
      else if (infoLogLevel == "fatal")
        lvl = rocksdb::InfoLogLevel::FATAL_LEVEL;
      else if (infoLogLevel == "header")
        lvl = rocksdb::InfoLogLevel::HEADER_LEVEL;
      else
        napi_throw_error(env, nullptr, "invalid log level");

      dbOptions.info_log_level = lvl;
    } else {
      // In some places RocksDB checks this option to see if it should prepare
      // debug information (ahead of logging), so set it to the highest level.
      dbOptions.info_log_level = rocksdb::InfoLogLevel::HEADER_LEVEL;
      dbOptions.info_log.reset(new NullLogger());
    }

    NAPI_STATUS_THROWS(InitOptions(env, dbOptions, options));

    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;

    bool hasColumns;
    NAPI_STATUS_THROWS(napi_has_named_property(env, options, "columns", &hasColumns));

    if (hasColumns) {
      napi_value columns;
      NAPI_STATUS_THROWS(napi_get_named_property(env, options, "columns", &columns));

      napi_value keys;
      NAPI_STATUS_THROWS(napi_get_property_names(env, columns, &keys));

      uint32_t len;
      NAPI_STATUS_THROWS(napi_get_array_length(env, keys, &len));

      descriptors.resize(len);
      for (uint32_t n = 0; n < len; ++n) {
        napi_value key;
        NAPI_STATUS_THROWS(napi_get_element(env, keys, n, &key));

        napi_value column;
        NAPI_STATUS_THROWS(napi_get_property(env, columns, key, &column));

        NAPI_STATUS_THROWS(InitOptions(env, descriptors[n].options, column));

        NAPI_STATUS_THROWS(GetValue(env, key, descriptors[n].name));
      }
    }

    auto callback = argv[2];

    runAsync<std::vector<rocksdb::ColumnFamilyHandle*>>(
        "leveldown.open_for_read_only", env, callback,
        [=](auto& handles) {
          assert(!database->db);

          rocksdb::DB* db = nullptr;

          const auto status = descriptors.empty()
                                  ? rocksdb::DB::OpenForReadOnly(dbOptions, database->location, &db)
                                  : rocksdb::DB::OpenForReadOnly(dbOptions, database->location, descriptors, &handles, &db);

          database->db.reset(db);

          return status;
        },
        [=](auto& handles, auto env, auto& argv) {
          argv.resize(2);

          NAPI_STATUS_RETURN(napi_create_object(env, &argv[1]));

          for (size_t n = 0; n < handles.size(); ++n) {
            ColumnFamily column;
            column.handle = handles[n];
            column.descriptor = descriptors[n];
            database->columns[column.handle->GetID()] = column;
          }

          napi_value columns = argv[1];
          for (auto& [id, column] : database->columns) {
            napi_value val;
            NAPI_STATUS_RETURN(napi_create_external(env, column.handle, nullptr, nullptr, &val));
            NAPI_STATUS_RETURN(napi_set_named_property(env, columns, column.descriptor.name.c_str(), val));
          }

          return napi_ok;
        });
  }

  return 0;
}

NAPI_METHOD(db_compact_range) {
  NAPI_ARGV(2);

  Database* database;
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&database)));

  std::optional<std::string> start;
  std::optional<std::string> end;

  NAPI_STATUS_THROWS(GetProperty(env, argv[1], "start", start));
  NAPI_STATUS_THROWS(GetProperty(env, argv[1], "end", end));

  rocksdb::CompactRangeOptions options;

  rocksdb::Slice* begin = start ? new rocksdb::Slice(*start) : nullptr;
  rocksdb::Slice* finish = end ? new rocksdb::Slice(*end) : nullptr;

  rocksdb::Status status = database->db->CompactRange(options, begin, finish);

  delete begin;
  delete finish;

  ROCKS_STATUS_THROWS_NAPI(status);

  return 0;
}

NAPI_INIT() {
  NAPI_EXPORT_FUNCTION(db_init);
  NAPI_EXPORT_FUNCTION(db_open);
  NAPI_EXPORT_FUNCTION(db_get_identity);
  NAPI_EXPORT_FUNCTION(db_get_handle);
  NAPI_EXPORT_FUNCTION(db_get_location);
  NAPI_EXPORT_FUNCTION(db_close);
  NAPI_EXPORT_FUNCTION(db_get_many);
  NAPI_EXPORT_FUNCTION(db_get_many_sync);
  NAPI_EXPORT_FUNCTION(db_clear);
  NAPI_EXPORT_FUNCTION(db_get_property);
  NAPI_EXPORT_FUNCTION(db_get_latest_sequence);
  NAPI_EXPORT_FUNCTION(db_query);

  NAPI_EXPORT_FUNCTION(iterator_init);
  NAPI_EXPORT_FUNCTION(iterator_seek);
  NAPI_EXPORT_FUNCTION(iterator_close);
  NAPI_EXPORT_FUNCTION(iterator_nextv);
  NAPI_EXPORT_FUNCTION(iterator_nextv_sync);

  NAPI_EXPORT_FUNCTION(updates_init);
  NAPI_EXPORT_FUNCTION(updates_close);
  NAPI_EXPORT_FUNCTION(updates_next);

  NAPI_EXPORT_FUNCTION(batch_init);
  NAPI_EXPORT_FUNCTION(batch_put);
  NAPI_EXPORT_FUNCTION(batch_put_log_data);
  NAPI_EXPORT_FUNCTION(batch_del);
  NAPI_EXPORT_FUNCTION(batch_clear);
  NAPI_EXPORT_FUNCTION(batch_write);
  NAPI_EXPORT_FUNCTION(batch_merge);
  NAPI_EXPORT_FUNCTION(batch_count);
  NAPI_EXPORT_FUNCTION(batch_iterate);
  NAPI_EXPORT_FUNCTION(db_open_for_read_only);
  NAPI_EXPORT_FUNCTION(db_compact_range);
}
