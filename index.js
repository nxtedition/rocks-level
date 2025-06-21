'use strict'

const { fromCallback } = require('catering')
const { AbstractLevel } = require('abstract-level')
const ModuleError = require('module-error')
const binding = require('./binding')
const { ChainedBatch } = require('./chained-batch')
const { Iterator } = require('./iterator')
const fs = require('node:fs')
const assert = require('node:assert')

const kContext = Symbol('context')
const kColumns = Symbol('columns')
const kPromise = Symbol('promise')
const kRefs = Symbol('refs')
const kPendingClose = Symbol('pendingClose')

const { kRef, kUnref } = require('./util')

const kEmpty = {}

class RocksLevel extends AbstractLevel {
  constructor (locationOrHandle, options) {
    super({
      encodings: {
        buffer: true,
        utf8: true
      },
      seek: true,
      additionalMethods: {
        updates: true,
        query: true
      }
    }, options)

    this[kContext] = binding.db_init(locationOrHandle)
    this[kColumns] = {}

    this[kRefs] = 0
    this[kPendingClose] = null
  }

  static async open (...args) {
    const db = new this(...args)
    await db.open()
    return db
  }

  get sequence () {
    return binding.db_get_latest_sequence(this[kContext])
  }

  get columns () {
    return this[kColumns]
  }

  get handle () {
    // TODO (fix): Support returning handle even if not open yet...
    assert(this.status === 'open', 'Database is not open')

    return binding.db_get_handle(this[kContext])
  }

  get location () {
    return binding.db_get_location(this[kContext])
  }

  _open (options, callback) {
    const doOpen = () => {
      let columns
      try {
        columns = binding.db_open(this[kContext], options, (err, columns) => {
          if (err) {
            callback(err)
          } else {
            this[kColumns] = columns
            callback(null)
          }
        })
      } catch (err) {
        callback(err)
      }

      if (columns) {
        this[kColumns] = columns
        callback(null)
      }
    }

    if (options.createIfMissing) {
      fs.mkdir(this.location, { recursive: true }, (err) => {
        if (err) {
          callback(err)
        } else {
          doOpen()
        }
      })
    } else {
      doOpen()
    }
  }

  [kRef] () {
    this[kRefs]++
  }

  [kUnref] () {
    this[kRefs]--
    if (this[kRefs] === 0 && this[kPendingClose]) {
      process.nextTick(this[kPendingClose])
    }
  }

  _close (callback) {
    if (this[kRefs]) {
      this[kPendingClose] = callback
    } else {
      binding.db_close(this[kContext], callback)
    }
  }

  _put (key, value, options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      const batch = this.batch()
      batch.put(key, value, options ?? kEmpty)
      batch.write(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _get (key, options, callback) {
    callback = fromCallback(callback, kPromise)

    this._getMany([key], options ?? kEmpty, (err, val) => {
      if (err) {
        callback(err)
      } else if (val[0] === undefined) {
        callback(Object.assign(new Error('not found'), {
          code: 'LEVEL_NOT_FOUND'
        }))
      } else {
        callback(null, val[0])
      }
    })

    return callback[kPromise]
  }

  _getMany (keys, options, callback) {
    return this._getManyAsync(keys, options, callback)
  }

  _getManyAsync (keys, options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      this[kRef]()
      binding.db_get_many(this[kContext], keys, options ?? kEmpty, (err, val) => {
        this[kUnref]()
        if (err) {
          callback(err)
        } else {
          callback(null, val)
        }
      })
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _getManySync (keys, options) {
    if (keys.some(key => typeof key === 'string')) {
      keys = keys.map(key => typeof key === 'string' ? Buffer.from(key) : key)
    }

    return binding.db_get_many_sync(this[kContext], keys, options ?? kEmpty)
  }

  _del (key, options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      const batch = this.batch()
      batch.del(key, options ?? kEmpty)
      batch.write(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _clear (options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      // TODO (fix): Use batch + DeleteRange...
      binding.db_clear(this[kContext], options ?? kEmpty)
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _chainedBatch () {
    return new ChainedBatch(this, this[kContext])
  }

  _batch (operations, options, callback) {
    callback = fromCallback(callback, kPromise)

    const batch = this._chainedBatch()
    for (const { type, key, value, column } of operations) {
      if (type === 'del') {
        batch._del(key, { column })
      } else if (type === 'put') {
        batch._put(key, value, { column })
      } else {
        assert(false)
      }
    }
    batch._write(options, callback)

    return callback[kPromise]
  }

  _iterator (options) {
    return new Iterator(this, this[kContext], options ?? kEmpty)
  }

  get identity () {
    return binding.db_get_identity(this[kContext])
  }

  getProperty (property) {
    if (typeof property !== 'string') {
      throw new TypeError("The first argument 'property' must be a string")
    }

    // Is synchronous, so can't be deferred
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    return binding.db_get_property(this[kContext], property)
  }

  async query (options) {
    return this.querySync(options)
  }

  querySync (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    return binding.db_query(this[kContext], options ?? kEmpty)
  }

  async * updates (options) {
    yield * this.updatesSync(options)
  }

  * updatesSync (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    const handle = binding.updates_init(this[kContext], options)
    try {
      while (true) {
        const value = binding.updates_next(handle)
        if (!value) {
          break
        }
        yield value
      }
    } finally {
      binding.updates_close(handle)
    }
  }

  openForReadOnly (options, callback) {
    if (callback === undefined) {
      callback = options
      options = {}
    }
    const doOpen = () => {
      let columns
      try {
        columns = binding.db_open_for_read_only(this[kContext], options, (err, columns) => {
          if (err) {
            callback?.(err)
          } else {
            this[kColumns] = columns
            const procId = setInterval(() => {
              if (this.status === 'opening') {
                /**
                 * Wait until db opens
                 */
              } else {
                clearInterval(procId)
                callback?.(null)
              }
            }, 1)
          }
        })
      } catch (err) {
        callback?.(err)
      }
      if (columns) {
        this[kColumns] = columns
        callback?.(null)
      }
    }
    doOpen()
  }

  compactRange (options = {}) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    return binding.db_compact_range(this[kContext], options)
  }
}

exports.RocksLevel = RocksLevel
