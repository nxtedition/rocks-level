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
const kRef = Symbol('ref')
const kUnref = Symbol('unref')
const kRefs = Symbol('refs')
const kPendingClose = Symbol('pendingClose')

const EMPTY = {}

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
      batch.put(key, value, options ?? EMPTY)
      batch.write(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _get (key, options, callback) {
    callback = fromCallback(callback, kPromise)

    this._getMany([key], options ?? EMPTY, (err, val) => {
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
    callback = fromCallback(callback, kPromise)

    const { keyEncoding, valueEncoding } = options ?? EMPTY

    if (keyEncoding !== 'buffer') {
      keys = keys.map(key => typeof key === 'string' ? Buffer.from(key) : key)
    }

    try {
      this[kRef]()
      binding.db_get_many(this[kContext], keys, options ?? EMPTY, (err, arrayBuffer) => {
        // const view = new DataView(arrayBuffer)
        // const result = []

        // let offset = 0
        // while (offset < arrayBuffer.byteLength) {
        //   const length = view.getInt32(offset, true)
        //   offset += 8

        //   const buffer = Buffer.from(arrayBuffer, offset, length)

        //   result.push(valueEncoding === 'buffer' ? buffer : buffer.toString(valueEncoding))

        //   offset += length
        //   if (offset & 0x7) {
        //     offset |= 0x7
        //     offset++
        //   }
        // }

        callback(err, result)
        this[kUnref]()
      })
    } catch (err) {
      process.nextTick(callback, err)
      this[kUnref]()
    }

    return callback[kPromise]
  }

  _getMergeOperands (key, options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      this[kRef]()
      binding.db_get_merge_operands(this[kContext], key, options ?? EMPTY, (err, val) => {
        callback(err, val)
        this[kUnref]()
      })
    } catch (err) {
      process.nextTick(callback, err)
      this[kUnref]()
    }

    return callback[kPromise]
  }

  _del (key, options, callback) {
    callback = fromCallback(callback, kPromise)

    try {
      const batch = this.batch()
      batch.del(key, options ?? EMPTY)
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
      binding.db_clear(this[kContext], options ?? EMPTY)
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _chainedBatch () {
    return new ChainedBatch(this, this[kContext], (batch, context, options, callback) => {
      try {
        this[kRef]()
        const sync = binding.batch_write(this[kContext], context, options, (err) => {
          this[kUnref]()
          callback(err)
        })
        if (sync) {
          this[kUnref]()
          process.nextTick(callback, null)
        }
      } catch (err) {
        process.nextTick(callback, err)
      }
    })
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
    batch.write(callback)

    return callback[kPromise]
  }

  _iterator (options) {
    return new Iterator(this, this[kContext], options ?? EMPTY)
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
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    const context = binding.iterator_init(this[kContext], options ?? {})
    try {
      this[kRef]()
      return await new Promise((resolve, reject) => binding.iterator_nextv(context, options.limit, (err, rows, finished) => {
        if (err) {
          reject(err)
        } else {
          resolve({
            rows,
            finished
          })
        }
      }))
    } finally {
      binding.iterator_close(context)
      this[kUnref]()
    }
  }
}

exports.RocksLevel = RocksLevel
