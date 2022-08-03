'use strict'

const { AbstractLevel } = require('abstract-level')
const ModuleError = require('module-error')
const fs = require('fs')
const binding = require('./binding')
const { ChainedBatch } = require('./chained-batch')
const { Iterator } = require('./iterator')
const os = require('os')

const kContext = Symbol('context')
const kColumns = Symbol('columns')
const kLocation = Symbol('location')

class RocksLevel extends AbstractLevel {
  constructor (location, options, _) {
    // To help migrating to abstract-level
    if (typeof options === 'function' || typeof _ === 'function') {
      throw new ModuleError('The levelup-style callback argument has been removed', {
        code: 'LEVEL_LEGACY'
      })
    }

    if (typeof location !== 'string' || location === '') {
      throw new TypeError("The first argument 'location' must be a non-empty string")
    }

    options = {
      ...options, // TODO (fix): Other defaults...
      parallelism: options?.parallelism ?? Math.max(1, os.cpus().length / 2),
      createIfMissing: options?.createIfMissing ?? true,
      errorIfExists: options?.errorIfExists ?? false,
      walTTL: options?.walTTL ?? 0,
      walSizeLimit: options?.walSizeLimit ?? 0,
      walCompression: options?.walCompression ?? false,
      unorderedWrite: options?.unorderedWrite ?? false,
      manualWalFlush: options?.manualWalFlush ?? false,
      infoLogLevel: options?.infoLogLevel ?? ''
    }

    // TODO (fix): Check options.

    super({
      encodings: {
        buffer: true,
        utf8: true
      },
      seek: true,
      createIfMissing: true,
      errorIfExists: true,
      additionalMethods: {
        updates: true,
        query: true
      }
    }, options)

    this[kLocation] = location
    this[kContext] = binding.db_init()
    this[kColumns] = {}
  }

  get sequence () {
    return binding.db_get_latest_sequence(this[kContext])
  }

  get location () {
    return this[kLocation]
  }

  get columns () {
    return this[kColumns]
  }

  _open (options, callback) {
    const onOpen = (err, columns) => {
      if (err) {
        callback(err)
      } else {
        this[kColumns] = columns
        callback(null)
      }
    }
    if (options.createIfMissing) {
      fs.mkdir(this[kLocation], { recursive: true }, (err) => {
        if (err) return callback(err)
        binding.db_open(this[kContext], this[kLocation], options, onOpen)
      })
    } else {
      binding.db_open(this[kContext], this[kLocation], options, onOpen)
    }
  }

  _close (callback) {
    binding.db_close(this[kContext], callback)
  }

  _put (key, value, options, callback) {
    try {
      const batch = this.batch()
      batch.put(key, value, options)
      batch.write(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _get (key, options, callback) {
    this._getMany([key], options, (err, val) => {
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
  }

  _getMany (keys, options, callback) {
    try {
      binding.db_get_many(this[kContext], keys, options, callback)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _del (key, options, callback) {
    try {
      const batch = this.batch()
      batch.del(key, options)
      batch.write(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _clear (options, callback) {
    try {
      binding.db_clear(this[kContext], options)
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _chainedBatch () {
    return new ChainedBatch(this, (batch, context, options, callback) => {
      try {
        binding.batch_write(this[kContext], context, options)
        process.nextTick(callback, null)
      } catch (err) {
        process.nextTick(callback, err)
      }
    })
  }

  _batch (operations, options, callback) {
    try {
      binding.batch_do(this[kContext], operations, options)
      process.nextTick(callback, null)
    } catch (err) {
      process.nextTick(callback, err)
    }
  }

  _iterator (options) {
    return new Iterator(this, this[kContext], options)
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

  async getCurrentWALFile () {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    return binding.db_get_current_wal_file(this[kContext])
  }

  async getSortedWALFiles () {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    return binding.db_get_sorted_wal_files(this[kContext])
  }

  async flushWAL (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    binding.db_flush_wal(this[kContext], options)
  }

  async query (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    const context = binding.iterator_init(this[kContext], options)
    const resource = {
      callback: null,
      close (callback) {
        this.callback = callback
      }
    }

    try {
      this.attachResource(resource)

      const limit = options.limit ?? 1000
      return await new Promise((resolve, reject) => binding.iterator_nextv(context, limit, (err, rows, finished) => {
        if (err) {
          reject(err)
        } else {
          resolve({
            rows,
            sequence: binding.iterator_get_sequence(context),
            finished
          })
        }
      }))
    } finally {
      this.detachResource(resource)
      binding.iterator_close(context)
      if (resource.callback) {
        resource.callback()
      }
    }
  }

  async * updates (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    options = {
      since: options?.since ?? 0,
      keys: options?.keys ?? true,
      values: options?.values ?? true,
      data: options?.data ?? true,
      column: options?.column ?? null
    }

    // HACK: We don't properly check for nully column in binding.
    if (!options.column) {
      delete options.column
    }

    if (typeof options.since !== 'number') {
      throw new TypeError("'since' must be nully or a number")
    }

    if (typeof options.keys !== 'boolean') {
      throw new TypeError("'keys' must be nully or a boolean")
    }

    if (typeof options.values !== 'boolean') {
      throw new TypeError("'values' must be nully or a boolean")
    }

    if (typeof options.data !== 'boolean') {
      throw new TypeError("'data' must be nully or a boolean")
    }

    if (options.column !== undefined && typeof options.column !== 'object') {
      throw new TypeError("'column' must be nully or a object")
    }

    class Updates {
      constructor (db, options) {
        this.context = binding.updates_init(db[kContext], options)
        this.closed = false
        this.promise = null
        this.db = db
        this.db.attachResource(this)
      }

      async next () {
        if (this.closed) {
          return {}
        }

        this.promise = new Promise(resolve => binding.updates_next(this.context, (err, rows, sequence, count) => {
          this.promise = null
          if (err) {
            resolve(Promise.reject(err))
          } else {
            resolve({ rows, sequence, count })
          }
        }))

        return this.promise
      }

      async close (callback) {
        try {
          await this.promise
        } catch {
          // Do nothing...
        }

        try {
          if (!this.closed) {
            this.closed = true
            binding.updates_close(this.context)
          }

          if (callback) {
            process.nextTick(callback)
          }
        } catch (err) {
          if (callback) {
            process.nextTick(callback, err)
          } else {
            throw err
          }
        } finally {
          this.db.detachResource(this)
        }
      }
    }

    const updates = new Updates(this, options)
    try {
      while (true) {
        const entry = await updates.next()
        if (!entry.rows) {
          return
        }
        yield entry
      }
    } finally {
      await updates.close()
    }
  }
}

exports.RocksLevel = RocksLevel
