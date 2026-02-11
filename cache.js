'use strict'

const binding = require('./binding')

const kCacheContext = Symbol('cacheContext')

class RocksCache {
  constructor(optionsOrHandle = {}) {
    this[kCacheContext] = binding.cache_init(optionsOrHandle)
  }

  get handle () {
    return binding.cache_get_handle(this[kCacheContext])
  }
}

exports.RocksCache = RocksCache
