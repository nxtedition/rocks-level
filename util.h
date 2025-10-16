#pragma once

#include <assert.h>
#include <napi-macros.h>
#include <node_api.h>

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include <array>
#include <optional>
#include <string>

#define NAPI_STATUS_RETURN(call) \
  {                              \
    auto _status = (call);       \
    if (_status != napi_ok) {    \
      return _status;            \
    }                            \
  }

#define ROCKS_STATUS_THROWS_NAPI(call)        \
  {                                           \
    auto _status = (call);                    \
    if (!_status.ok()) {                      \
      napi_throw(env, ToError(env, _status)); \
      return NULL;                            \
    }                                         \
  }

#define ROCKS_STATUS_RETURN_NAPI(call)        \
  {                                           \
    auto _status = (call);                    \
    if (!_status.ok()) {                      \
      napi_throw(env, ToError(env, _status)); \
      return napi_pending_exception;          \
    }                                         \
  }

#define ROCKS_STATUS_RETURN(call) \
  {                               \
    auto _status = (call);        \
    if (!_status.ok()) {          \
      return _status;             \
    }                             \
  }

template <typename T>
static void Finalize(napi_env env, void* data, void* hint) {
  if (hint) {
    delete reinterpret_cast<T*>(hint);
  }
}

static void FinalizeFree(napi_env env, void* data, void* hint) {
  if (hint) {
    free(hint);
  }
}

static napi_value CreateError(napi_env env, const std::optional<std::string_view>& code, const std::string_view& msg) {
  napi_value codeValue = nullptr;
  if (code) {
    NAPI_STATUS_THROWS(napi_create_string_utf8(env, code->data(), code->size(), &codeValue));
  }
  napi_value msgValue;
  NAPI_STATUS_THROWS(napi_create_string_utf8(env, msg.data(), msg.size(), &msgValue));
  napi_value error;
  NAPI_STATUS_THROWS(napi_create_error(env, codeValue, msgValue, &error));
  return error;
}

static napi_value ToError(napi_env env, const rocksdb::Status& status) {
  if (status.ok()) {
    return 0;
  }

  const auto msg = status.ToString();

  if (status.IsNotFound()) {
    return CreateError(env, "LEVEL_NOT_FOUND", msg);
  } else if (status.IsCorruption()) {
    return CreateError(env, "LEVEL_CORRUPTION", msg);
  } else if (status.IsTryAgain()) {
    return CreateError(env, "LEVEL_TRYAGAIN", msg);
  } else if (status.IsIOError()) {
    if (msg.find("IO error: lock ") != std::string::npos) {  // env_posix.cc
      return CreateError(env, "LEVEL_LOCKED", msg);
    } else if (msg.find("IO error: LockFile ") != std::string::npos) {  // env_win.cc
      return CreateError(env, "LEVEL_LOCKED", msg);
    } else if (msg.find("IO error: While lock file") != std::string::npos) {  // env_mac.cc
      return CreateError(env, "LEVEL_LOCKED", msg);
    } else {
      return CreateError(env, "LEVEL_IO_ERROR", msg);
    }
  }

  return CreateError(env, {}, msg);
}

static napi_status GetString(napi_env env, napi_value from, rocksdb::Slice& to) {
  bool isBuffer;
  NAPI_STATUS_RETURN(napi_is_buffer(env, from, &isBuffer));

  if (isBuffer) {
    char* buf = nullptr;
    size_t length = 0;
    NAPI_STATUS_RETURN(napi_get_buffer_info(env, from, reinterpret_cast<void**>(&buf), &length));
    to = {buf, length};
    return napi_ok;
  }

  napi_valuetype type;
  NAPI_STATUS_RETURN(napi_typeof(env, from, &type));

  if (type == napi_object) {
    // Slice
    napi_value value;
    NAPI_STATUS_RETURN(napi_get_named_property(env, from, "buffer", &value));

    char* buf = nullptr;
    size_t length = 0;
    NAPI_STATUS_RETURN(napi_get_buffer_info(env, value, reinterpret_cast<void**>(&buf), &length));

    int pos = 0;
    {
      napi_value property;
      NAPI_STATUS_RETURN(napi_get_named_property(env, from, "byteOffset", &property));
      NAPI_STATUS_RETURN(napi_get_value_int32(env, property, &pos));
    }

    if (pos < 0 || pos > length) {
      return napi_invalid_arg;
    }

    int len = length;
    {
      napi_value property;
      NAPI_STATUS_RETURN(napi_get_named_property(env, from, "byteLength", &property));
      NAPI_STATUS_RETURN(napi_get_value_int32(env, property, &len));
    }

    if (len < 0 || len > length) {
      return napi_invalid_arg;
    }

    if (pos + len > length) {
      return napi_invalid_arg;
    }

    to = {buf + pos, static_cast<size_t>(len)};

    return napi_ok;
  }

  return napi_invalid_arg;
}

static napi_status GetString(napi_env env, napi_value from, std::string& to) {
  napi_valuetype type;
  NAPI_STATUS_RETURN(napi_typeof(env, from, &type));

  if (type == napi_string) {
    size_t length = 0;
    NAPI_STATUS_RETURN(napi_get_value_string_utf8(env, from, nullptr, 0, &length));
    // TODO (perf): Avoid zero initialization...
    to.resize(length, '\0');
    NAPI_STATUS_RETURN(napi_get_value_string_utf8(env, from, &to[0], length + 1, &length));
    to[length] = 0;
  } else {
    rocksdb::Slice slice;
    NAPI_STATUS_RETURN(GetString(env, from, slice));
    to = slice.ToString();
  }

  return napi_ok;
}

static napi_status GetString(napi_env env, napi_value from, rocksdb::PinnableSlice& to) {
  napi_valuetype type;
  NAPI_STATUS_RETURN(napi_typeof(env, from, &type));

  if (type == napi_string) {
    size_t length = 0;
    NAPI_STATUS_RETURN(napi_get_value_string_utf8(env, from, nullptr, 0, &length));
    to.GetSelf()->resize(length, '\0');
    NAPI_STATUS_RETURN(napi_get_value_string_utf8(env, from, to.GetSelf()->data(), length + 1, &length));
    to.PinSelf();
  } else {
    rocksdb::Slice slice;
    NAPI_STATUS_RETURN(GetString(env, from, slice));
    to.PinSelf(std::move(slice));
  }

  return napi_ok;
}

enum class Encoding { Invalid, Buffer, String };

static napi_status GetValue(napi_env env, napi_value value, bool& result) {
  return napi_get_value_bool(env, value, &result);
}

static napi_status GetValue(napi_env env, napi_value value, int& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<int>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, long& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<long>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, long long& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<long long>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, unsigned int& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<unsigned int>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, unsigned long& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<unsigned long>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, unsigned long long& result) {
  int64_t result2;
  NAPI_STATUS_RETURN(napi_get_value_int64(env, value, &result2));
  result = static_cast<unsigned long long>(result2);
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, double& result) {
  NAPI_STATUS_RETURN(napi_get_value_double(env, value, &result));
  return napi_ok;
}

static napi_status GetValue(napi_env env, napi_value value, std::string& result) {
  return GetString(env, value, result);
}

static napi_status GetValue(napi_env env, napi_value value, rocksdb::PinnableSlice& result) {
  return GetString(env, value, result);
}

static napi_status GetValue(napi_env env, napi_value value, rocksdb::Slice& result) {
  return GetString(env, value, result);
}

static napi_status GetValue(napi_env env, napi_value value, rocksdb::ColumnFamilyHandle*& result) {
  return napi_get_value_external(env, value, reinterpret_cast<void**>(&result));
}

static napi_status GetValue(napi_env env, napi_value value, std::shared_ptr<rocksdb::Cache>*& result) {
  return napi_get_value_external(env, value, reinterpret_cast<void**>(&result));
}

static napi_status GetValue(napi_env env, napi_value value, Encoding& result) {
  size_t size;
  NAPI_STATUS_RETURN(napi_get_value_string_utf8(env, value, nullptr, 0, &size));

  if (size == 6) {
    result = Encoding::Buffer;
    return napi_ok;
  } else {
    result = Encoding::String;
    return napi_ok;
  }

  return napi_invalid_arg;
}

static napi_status GetValue(napi_env env,
                            napi_value value,
                            rocksdb::BlockBasedTableOptions::PrepopulateBlockCache& result) {
  std::string str;

  if (GetValue(env, value, str) == napi_ok) {
    if (str == "flushOnly") {
      result = rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushOnly;
      return napi_ok;
    } else if (str == "disable") {
      result = rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kDisable;
      return napi_ok;
    } else {
      return napi_invalid_arg;
    }
  }

  bool boolean;
  if (GetValue(env, value, boolean) == napi_ok) {
    result = boolean ? rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kFlushOnly
                     : rocksdb::BlockBasedTableOptions::PrepopulateBlockCache::kDisable;
    return napi_ok;
  }

  return napi_invalid_arg;
}

static napi_status GetValue(napi_env env, napi_value value, rocksdb::PrepopulateBlobCache& result) {
  std::string str;

  if (GetValue(env, value, str) == napi_ok) {
    if (str == "flushOnly") {
      result = rocksdb::PrepopulateBlobCache::kFlushOnly;
      return napi_ok;
    } else if (str == "disable") {
      result = rocksdb::PrepopulateBlobCache::kDisable;
      return napi_ok;
    } else {
      return napi_invalid_arg;
    }
  }

  bool boolean;
  if (GetValue(env, value, boolean) == napi_ok) {
    result = boolean ? rocksdb::PrepopulateBlobCache::kFlushOnly : rocksdb::PrepopulateBlobCache::kDisable;
    return napi_ok;
  }

  return napi_invalid_arg;
}

static napi_status GetValue(napi_env env, napi_value value, rocksdb::CompressionType& result) {
  std::string str;

  if (GetValue(env, value, str) == napi_ok) {
    if (str == "no") {
      result = rocksdb::CompressionType::kNoCompression;
      return napi_ok;
    } else if (str == "snappy") {
      result = rocksdb::CompressionType::kSnappyCompression;
      return napi_ok;
    } else if (str == "zlib") {
      result = rocksdb::CompressionType::kZlibCompression;
      return napi_ok;
    } else if (str == "bzip2") {
      result = rocksdb::CompressionType::kBZip2Compression;
      return napi_ok;
    } else if (str == "lz4") {
      result = rocksdb::CompressionType::kLZ4Compression;
      return napi_ok;
    } else if (str == "lz4hc") {
      result = rocksdb::CompressionType::kLZ4HCCompression;
      return napi_ok;
    } else if (str == "xpress") {
      result = rocksdb::CompressionType::kXpressCompression;
      return napi_ok;
    } else if (str == "zstd") {
      result = rocksdb::CompressionType::kZSTD;
      return napi_ok;
    } else {
      return napi_invalid_arg;
    }
  }

  bool boolean;
  if (GetValue(env, value, boolean) == napi_ok) {
    result = boolean ? rocksdb::kZSTD : rocksdb::kNoCompression;
    return napi_ok;
  }

  return napi_invalid_arg;
}

template <typename T>
static napi_status GetValue(napi_env env, napi_value value, std::optional<T>& result) {
  result = T{};
  return GetValue(env, value, *result);
}

template <typename T>
static napi_status GetProperty(napi_env env,
                               napi_value obj,
                               const std::string_view& key,
                               T& result,
                               bool required = false) {
  napi_valuetype objType;
  NAPI_STATUS_RETURN(napi_typeof(env, obj, &objType));

  if (objType == napi_undefined || objType == napi_null) {
    return required ? napi_invalid_arg : napi_ok;
  }

  if (objType != napi_object) {
    return napi_invalid_arg;
  }

  bool has = false;
  NAPI_STATUS_RETURN(napi_has_named_property(env, obj, key.data(), &has));

  if (!has) {
    return required ? napi_invalid_arg : napi_ok;
  }

  napi_value value;
  NAPI_STATUS_RETURN(napi_get_named_property(env, obj, key.data(), &value));

  napi_valuetype valueType;
  NAPI_STATUS_RETURN(napi_typeof(env, value, &valueType));

  if (valueType == napi_null || valueType == napi_undefined) {
    return required ? napi_invalid_arg : napi_ok;
  }

  return GetValue(env, value, result);
}

template <typename T>
napi_status Convert(napi_env env, T&& s, Encoding encoding, napi_value& result) {
  if (!s) {
    return napi_get_null(env, &result);
  } else if (encoding == Encoding::Buffer) {
    return napi_create_buffer_copy(env, s->size(), s->data(), nullptr, &result);
  } else if (encoding == Encoding::String) {
    return napi_create_string_utf8(env, s->data(), s->size(), &result);
  } else {
    return napi_invalid_arg;
  }
}

napi_status ConvertUnsafe(napi_env env, rocksdb::PinnableSlice&& s, Encoding encoding, napi_value& result) {
  if (encoding == Encoding::Buffer) {
    auto s2 = new rocksdb::PinnableSlice(std::move(s));
    return napi_create_external_buffer(env, s2->size(), const_cast<char*>(s2->data()), Finalize<rocksdb::PinnableSlice>,
                                       s2, &result);
  } else if (encoding == Encoding::String) {
    return napi_create_string_utf8(env, s.data(), s.size(), &result);
  } else {
    return napi_invalid_arg;
  }
}

napi_status Convert(napi_env env, rocksdb::PinnableSlice&& s, Encoding encoding, napi_value& result) {
  if (encoding == Encoding::Buffer) {
    return napi_create_buffer_copy(env, s.size(), s.data(), nullptr, &result);
  } else if (encoding == Encoding::String) {
    return napi_create_string_utf8(env, s.data(), s.size(), &result);
  } else {
    return napi_invalid_arg;
  }
}

template <typename State, typename T1, typename T2>
napi_status runAsync(State&& state,
                     const std::string& name,
                     napi_env env,
                     napi_value callback,
                     T1&& execute,
                     T2&& then) {
  struct Worker final {
    static void Execute(napi_env env, void* data) {
      auto worker = reinterpret_cast<Worker*>(data);
      worker->status = worker->execute(worker->state);
    }

    static void Complete(napi_env env, napi_status status, void* data) {
      auto worker = std::unique_ptr<Worker>(reinterpret_cast<Worker*>(data));

      NAPI_STATUS_THROWS_VOID(napi_open_handle_scope(env, &worker->scope));

      napi_value callback;
      NAPI_STATUS_THROWS_VOID(napi_get_reference_value(env, worker->callbackRef, &callback));

      napi_value global;
      NAPI_STATUS_THROWS_VOID(napi_get_global(env, &global));

      if (worker->status.ok()) {
        std::vector<napi_value> argv;

        argv.resize(1);
        NAPI_STATUS_THROWS_VOID(napi_get_null(env, &argv[0]));

        const auto ret = worker->then(worker->state, env, argv);

        if (ret == napi_ok) {
          NAPI_STATUS_THROWS_VOID(napi_call_function(env, global, callback, argv.size(), argv.data(), nullptr));
        } else {
          const napi_extended_error_info* errInfo = nullptr;
          NAPI_STATUS_THROWS_VOID(napi_get_last_error_info(env, &errInfo));
          auto err = CreateError(env, std::nullopt,
                                 !errInfo || !errInfo->error_message ? "empty error message" : errInfo->error_message);
          NAPI_STATUS_THROWS_VOID(napi_call_function(env, global, callback, 1, &err, nullptr));
        }
      } else {
        auto err = ToError(env, worker->status);
        NAPI_STATUS_THROWS_VOID(napi_call_function(env, global, callback, 1, &err, nullptr));
      }

      NAPI_STATUS_THROWS_VOID(napi_close_handle_scope(env, worker->scope));
      worker->scope = nullptr;
    }

    ~Worker() {
      if (callbackRef) {
        napi_delete_reference(env, callbackRef);
        callbackRef = nullptr;
      }
      if (asyncWork) {
        napi_delete_async_work(env, asyncWork);
        asyncWork = nullptr;
      }
      if (scope) {
        napi_close_handle_scope(env, scope);
        scope = nullptr;
      }
    }

    napi_env env = nullptr;

    typename std::decay<T1>::type execute;
    typename std::decay<T2>::type then;

    State state;

    napi_ref callbackRef = nullptr;
    napi_async_work asyncWork = nullptr;
    napi_handle_scope scope = nullptr;
    rocksdb::Status status = rocksdb::Status::OK();
  };

  auto worker =
      std::unique_ptr<Worker>(new Worker{env, std::forward<T1>(execute), std::forward<T2>(then), std::move(state)});

  NAPI_STATUS_RETURN(napi_create_reference(env, callback, 1, &worker->callbackRef));
  napi_value asyncResourceName;
  NAPI_STATUS_RETURN(napi_create_string_utf8(env, name.data(), name.size(), &asyncResourceName));
  NAPI_STATUS_RETURN(napi_create_async_work(env, callback, asyncResourceName, Worker::Execute, Worker::Complete,
                                            worker.get(), &worker->asyncWork));

  NAPI_STATUS_RETURN(napi_queue_async_work(env, worker->asyncWork));

  worker.release();

  return napi_ok;
}
template <typename State, typename T1, typename T2>
napi_status runAsync(const std::string& name, napi_env env, napi_value callback, T1&& execute, T2&& then) {
  return runAsync<State>(State{}, name, env, callback, std::forward<T1>(execute), std::forward<T2>(then));
}
