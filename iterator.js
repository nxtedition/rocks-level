'use strict'

const { fromCallback } = require('catering')
const { AbstractIterator } = require('abstract-level')
const assert = require('node:assert')
const { kRef, kUnref } = require('./util')

const binding = require('./binding')

const kPromise = Symbol('promise')
const kDB = Symbol('db')
const kContext = Symbol('context')
const kCache = Symbol('cache')
const kFinished = Symbol('finished')
const kFirst = Symbol('first')
const kPosition = Symbol('position')
const kBusy = Symbol('busy')

const kEmpty = []
const kForceSync = false

class Iterator extends AbstractIterator {
  constructor (db, context, options) {
    super(db, options)

    this[kContext] = binding.iterator_init_sync(context, options)

    this[kFirst] = true
    this[kCache] = kEmpty
    this[kFinished] = false
    this[kPosition] = 0
    this[kDB] = db
    this[kBusy] = false
  }

  _seek (target) {
    this._seekSync(target)
  }

  _close (callback) {
    return this._closeAsync(callback)
  }

  _end (callback) {
    this._close(callback)
  }

  // Undocumented, exposed for tests only
  get cached () {
    return (this[kCache].length - this[kPosition]) / 2
  }


  _next(callback) {
    assert(this[kContext])
    assert(!this[kBusy])

    if (this[kPosition] < this[kCache].length) {
      const key = this[kCache][this[kPosition]++]
      const val = this[kCache][this[kPosition]++]
      process.nextTick(callback, null, key, val)
    } else if (this[kFinished]) {
      process.nextTick(callback)
    } else {
      const size = this[kFirst] ? 1 : 1000
      this[kFirst] = false

      try {
        const { rows, finished } = binding.iterator_nextv_sync(this[kContext], size, null)
        this[kCache] = rows
        this[kFinished] = finished
        this[kPosition] = 0

        setImmediate(() => this._next(callback))
      } catch (err) {
        process.nextTick(callback, err)
      }
    }

    return this
  }

  _nextv(size, options, callback) {
    assert(this[kContext])
    assert(!this[kBusy])

    callback = fromCallback(callback, kPromise)

    this._nextvAsync(size, options, (err, val) => {
      if (err) {
        callback(err)
      } else {
        const { rows, finished } = val

        const entries = []
        for (let n = 0; n < rows.length; n += 2) {
          entries.push([rows[n + 0], rows[n + 1]])
        }

        callback(null, entries, finished)
      }
    })

    return callback[kPromise]
  }

  // nxt API

  _seekSync (target) {
    assert(this[kContext])
    assert(!this[kBusy])

    if (target.length === 0) {
      throw new Error('cannot seek() to an empty target')
    }

    this[kFirst] = true
    this[kCache] = kEmpty
    this[kFinished] = false
    this[kPosition] = 0

    binding.iterator_seek_sync(this[kContext], target)
  }

  _seekAsync (target, callback) {
    assert(this[kContext])
    assert(!this[kBusy])

    callback = fromCallback(callback, kPromise)

    this[kFirst] = true
    this[kCache] = kEmpty
    this[kFinished] = false
    this[kPosition] = 0

    try {
      binding.iterator_seek(this[kContext], target, (err) => {
        this[kBusy] = false
        this[kDB][kUnref]()

        if (err) {
          callback(err)
        } else {
          callback(null)
        }
      })
      this[kDB][kRef]()
      this[kBusy] = true
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _nextvSync (size, options) {
    assert(this[kContext])
    assert(!this[kBusy])

    if (this[kFinished]) {
      return { rows: [], finished: true }
    }

    const result = binding.iterator_nextv_sync(this[kContext], size, options)
    this[kFinished] = result.finished

    return result
  }

  _nextvAsync (size, options, callback) {
    assert(this[kContext])
    assert(!this[kBusy])

    callback = fromCallback(callback, kPromise)

    try {
      if (this[kFinished]) {
        process.nextTick(callback, null, { rows: [], finished: true })
      } else {
        binding.iterator_nextv(this[kContext], size, options, (err, result) => {
          this[kBusy] = false
          this[kDB][kUnref]()

          if (err) {
            callback(err)
          } else {
            this[kFinished] = result.finished
            callback(null, result)
          }
        })
        this[kBusy] = true
        this[kDB][kRef]()
      }
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }

  _closeSync () {
    this[kCache] = kEmpty

    if (this[kContext]) {
      binding.iterator_close_sync(this[kContext])
      this[kContext] = null
    }
  }

  _closeAsync (callback) {
    callback = fromCallback(callback, kPromise)

    try {
      this._closeSync()
      process.nextTick(callback)
    } catch (err) {
      process.nextTick(callback, err)
    }

    return callback[kPromise]
  }
}

exports.Iterator = Iterator
