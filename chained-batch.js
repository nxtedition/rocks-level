'use strict'

const { AbstractChainedBatch } = require('abstract-level')
const binding = require('./binding')
const ModuleError = require('module-error')
const assert = require('node:assert')

const kBatchContext = Symbol('batchContext')
const kDbContext = Symbol('dbContext')
const EMPTY = {}

class ChainedBatch extends AbstractChainedBatch {
  constructor (db, context) {
    super(db)

    this[kDbContext] = context
    this[kBatchContext] = binding.batch_init()
  }

  _put (key, value, options) {
    assert(this[kBatchContext])

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

  _putLogData (blob) {
    assert(this[kBatchContext])

    if (blob === null || blob === undefined) {
      throw new ModuleError('Blob cannot be null or undefined', {
        code: 'LEVEL_INVALID_KEY'
      })
    }

    blob = typeof blob === 'string' ? Buffer.from(blob) : blob

    binding.batch_put_log_data(this[kBatchContext], blob)
  }

  _del (key, options) {
    assert(this[kBatchContext])

    if (key === null || key === undefined) {
      throw new ModuleError('Key cannot be null or undefined', {
        code: 'LEVEL_INVALID_KEY'
      })
    }

    key = typeof key === 'string' ? Buffer.from(key) : key

    binding.batch_del(this[kBatchContext], key, options ?? EMPTY)
  }

  _clear () {
    assert(this[kBatchContext])

    binding.batch_clear(this[kBatchContext])
  }

  _write (options, callback) {
    assert(this[kBatchContext])

    try {
      this._writeSync(options)
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _writeSync (options) {
    assert(this[kBatchContext])

    binding.batch_write(this[kDbContext], this[kBatchContext], options ?? EMPTY)
  }

  _close (callback) {
    assert(this[kBatchContext])

    try {
      this._closeSync()
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _closeSync () {
    if (this[kBatchContext]) {
      binding.batch_clear(this[kBatchContext])
      this[kBatchContext] = null
    }
  }

  get length () {
    assert(this[kBatchContext])

    return binding.batch_count(this[kBatchContext])
  }

  _merge (key, value, options) {
    assert(this[kBatchContext])

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
    if (!this[kBatchContext]) {
      return []
    }

    return binding.batch_iterate(this[kDbContext], this[kBatchContext], {
      keys: true,
      values: true,
      data: true,
      ...options
    })
  }
}

exports.ChainedBatch = ChainedBatch
