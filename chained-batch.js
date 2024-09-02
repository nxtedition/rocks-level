'use strict'

const { AbstractChainedBatch } = require('abstract-level')
const binding = require('./binding')
const ModuleError = require('module-error')
const { fromCallback } = require('catering')

const kWrite = Symbol('write')
const kBatchContext = Symbol('batchContext')
const kDbContext = Symbol('dbContext')
const kPromise = Symbol('promise')

const EMPTY = {}

class ChainedBatch extends AbstractChainedBatch {
  constructor (db, context, write) {
    super(db)

    this[kWrite] = write
    this[kDbContext] = context
    this[kBatchContext] = binding.batch_init()
  }

  _put (key, value, options) {
    if (key === null || key === undefined) {
      throw new ModuleError('Key cannot be null or undefined', {
        code: 'LEVEL_INVALID_KEY'
      })
    }

    if (value === null || value === undefined) {
      throw new ModuleError('value cannot be null or undefined', {
        code: 'LEVEL_INVALID_VALUE'
      })
    }

    key = typeof key === 'string' ? Buffer.from(key) : key
    value = typeof value === 'string' ? Buffer.from(value) : value

    binding.batch_put(this[kBatchContext], key, value, options ?? EMPTY)
  }

  _del (key, options) {
    if (key === null || key === undefined) {
      throw new ModuleError('Key cannot be null or undefined', {
        code: 'LEVEL_INVALID_KEY'
      })
    }

    key = typeof key === 'string' ? Buffer.from(key) : key

    binding.batch_del(this[kBatchContext], key, options ?? EMPTY)
  }

  _clear () {
    binding.batch_clear(this[kBatchContext])
  }

  _write (options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      binding.batch_write_sync(this[kDbContext], this[kBatchContext], options)
      process.nextTick(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _writeSync (options) {
    binding.batch_write_sync(this[kDbContext], this[kBatchContext], options)
  }

  _close (callback) {
    process.nextTick(callback)
  }

  get length () {
    return binding.batch_count(this[kBatchContext])
  }

  _merge (key, value, options) {
    if (key === null || key === undefined) {
      throw new ModuleError('Key cannot be null or undefined', {
        code: 'LEVEL_INVALID_KEY'
      })
    }

    if (value === null || value === undefined) {
      throw new ModuleError('value cannot be null or undefined', {
        code: 'LEVEL_INVALID_VALUE'
      })
    }

    key = typeof key === 'string' ? Buffer.from(key) : key
    value = typeof value === 'string' ? Buffer.from(value) : value

    binding.batch_merge(this[kBatchContext], key, value, options ?? EMPTY)
  }

  * [Symbol.iterator] () {
    const rows = this.toArray()
    for (let n = 0; n < rows.length; n += 4) {
      yield {
        type: rows[n + 0],
        key: rows[n + 1],
        value: rows[n + 2]
      }
    }
  }

  toArray (options) {
    return binding.batch_iterate(this[kDbContext], this[kBatchContext], {
      keys: true,
      values: true,
      data: true,
      ...options
    })
  }
}

exports.ChainedBatch = ChainedBatch
