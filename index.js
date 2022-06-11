'use strict'

const { AbstractLevel } = require('abstract-level')
const ModuleError = require('module-error')
const fs = require('fs')
const binding = require('./binding')
const { ChainedBatch } = require('./chained-batch')
const { Iterator } = require('./iterator')

const kContext = Symbol('context')
const kLocation = Symbol('location')
const kColumns = Symbol('columns')

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

    super({
      encodings: {
        buffer: true,
        utf8: true
      },
      seek: true,
      createIfMissing: true,
      errorIfExists: true,
      additionalMethods: {
        createColumn: true,
        closeColumn: true,
        updates: true,
        query: true
      }
    }, options)

    this[kLocation] = location
    this[kContext] = binding.db_init()
    this[kColumns] = new Set()
  }

  get location () {
    return this[kLocation]
  }

  _open (options, callback) {
    if (options.createIfMissing) {
      fs.mkdir(this[kLocation], { recursive: true }, (err) => {
        if (err) return callback(err)
        binding.db_open(this[kContext], this[kLocation], options, callback)
      })
    } else {
      binding.db_open(this[kContext], this[kLocation], options, callback)
    }
  }

  _close (callback) {
    for (const column of this[kColumns]) {
      binding.column_close(column)
    }
  
    binding.db_close(this[kContext], callback)
  }

  _put (key, value, options, callback) {
    process.nextTick(callback, binding.db_put(this[kContext], key, value, options))
  }

  _get (key, options, callback) {
    binding.db_get(this[kContext], key, options, callback)
  }

  _getMany (keys, options, callback) {
    binding.db_get_many(this[kContext], keys, options, callback)
  }

  _del (key, options, callback) {
    process.nextTick(callback, binding.db_del(this[kContext], key, options))
  }

  _clear (options, callback) {
    process.nextTick(callback, binding.db_clear(this[kContext], options))
  }

  _chainedBatch () {
    return new ChainedBatch(this, this[kContext])
  }

  _batch (operations, options, callback) {
    process.nextTick(callback, binding.batch_do(this[kContext], operations, options))
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

  async createColumn (name, options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    const column = binding.column_init(this[kContext], name, options || {})
    
    this[kColumns].add(column)
    
    return column
  }

  async * query (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
    }

    class Query {
      constructor (db, options) {
        this.context = binding.iterator_init(db[kContext], options)
        this.closed = false
        this.promise = null
        this.sequence = binding.iterator_get_sequence(this.context)
        this.db = db
        this.db.attachResource(this)
      }

      async next () {
        if (this.closed) {
          return {}
        }

        this.promise = new Promise(resolve => binding.iterator_nextv(this.context, 1000, (err, rows, finished) => {
          this.promise = null
          if (err) {
            resolve(Promise.reject(err))
          } else {
            resolve({
              finished,
              rows,
              sequence: this.sequence
            })
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
            binding.iterator_close(this.context)
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

    const query = new Query(this, options)
    try {
      while (true) {
        const { finished, rows, sequence } = await query.next()

        yield { rows, sequence }

        if (finished) {
          return
        }
      }
    } finally {
      await query.close()
    }
  }

  async * updates (options) {
    if (this.status !== 'open') {
      throw new ModuleError('Database is not open', {
        code: 'LEVEL_DATABASE_NOT_OPEN'
      })
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

        this.promise = new Promise(resolve => binding.updates_next(this.context, (err, rows, sequence) => {
          this.promise = null
          if (err) {
            resolve(Promise.reject(err))
          } else {
            resolve({ rows, sequence })
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
        const { rows, sequence } = await updates.next()
        if (!rows) {
          return
        }
        yield { rows, sequence }
      }
    } finally {
      await updates.close()
    }
  }
}

exports.RocksLevel = RocksLevel
