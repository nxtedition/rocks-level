'use strict'

const test = require('tape')
const tempy = require('tempy')
const { RocksLevel } = require('..')

// Regression: close() must perform the native db_close — releasing the RocksDB
// directory lock — even when it is *deferred* because an async op
// (compactRange / flushWAL / array batch / updates) still holds a ref. The
// deferred drain path previously invoked the abstract-level callback without
// calling binding.db_close, so the native DB and its lock leaked until GC. The
// observable symptom is that the same location cannot be reopened.

async function reopen (location) {
  // Throws (lock error, surfaced as LEVEL_DATABASE_NOT_OPEN) if the previous
  // db's native handle and directory lock were leaked.
  const db = new RocksLevel(location)
  await db.open()
  await db.close()
}

test('close() releases the directory lock on the direct (no in-flight op) path', async function (t) {
  const location = tempy.directory()
  const db = new RocksLevel(location)
  await db.open()
  await db.put('x', '1')
  await db.close()
  await reopen(location)
  t.pass('reopened same location after a direct close')
  t.end()
})

test('close() releases the directory lock when deferred by an in-flight op', async function (t) {
  const ops = {
    compactRange: (db) => db.compactRange(),
    flushWAL: (db) => db.flushWAL(),
    'array batch': (db) => db.batch([{ type: 'put', key: 'a', value: '1' }])
  }

  // Hold every closed db reference until the end so GC can't release the lock
  // for us — the close() itself must do it.
  const closed = []

  for (const [name, startOp] of Object.entries(ops)) {
    const location = tempy.directory()
    const db = new RocksLevel(location)
    closed.push(db)
    await db.open()
    await db.put('x', '1')

    const inflight = startOp(db) // takes a db ref -> close() is deferred
    await db.close() // must still perform the native close on drain
    await inflight.catch(() => {})

    await reopen(location)
    t.pass(`reopened after deferred close (${name})`)
  }

  t.equal(closed.length, 3, 'all source dbs kept referenced (no GC-assisted unlock)')
  t.end()
})
