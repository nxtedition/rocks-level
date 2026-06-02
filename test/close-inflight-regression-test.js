'use strict'

const test = require('tape')
const testCommon = require('./common')

// Number of open/op/close cycles per scenario. The bugs are races, so we repeat
// to give them a chance to surface; on the fixed code every cycle is clean.
const ITERATIONS = 10

async function seed (db, n) {
  const ops = []
  for (let i = 0; i < n; i++) {
    ops.push({ type: 'put', key: 'k' + i, value: 'v' + i })
  }
  await db.batch(ops)
}

// #2: close() must defer the native db teardown (which frees the rocksdb::DB and
// column handles on a worker thread) until in-flight async ops that run on other
// worker threads have completed; otherwise it frees the DB underneath them (UAF).

test('close() waits for in-flight array batch writes', async function (t) {
  for (let i = 0; i < ITERATIONS; i++) {
    const db = testCommon.factory()
    await db.open()
    const writes = []
    for (let j = 0; j < 8; j++) {
      writes.push(db.batch([{ type: 'put', key: 'k' + j, value: 'v' + j }]))
    }
    await db.close()
    await Promise.all(writes) // every write ran against a live db
  }
  t.pass('survived ' + ITERATIONS + ' batch+close iterations without UAF')
  t.end()
})

test('close() waits for in-flight compactRange', async function (t) {
  for (let i = 0; i < ITERATIONS; i++) {
    const db = testCommon.factory()
    await db.open()
    await seed(db, 20)
    const p = db.compactRange()
    await db.close()
    await p
  }
  t.pass('survived compactRange+close')
  t.end()
})

test('close() waits for in-flight flushWAL', async function (t) {
  for (let i = 0; i < ITERATIONS; i++) {
    const db = testCommon.factory()
    await db.open()
    await seed(db, 5)
    const p = db.flushWAL()
    await db.close()
    await p
  }
  t.pass('survived flushWAL+close')
  t.end()
})

// #3: closing an iterator (triggered by db.close()) while an async nextv is in
// flight on a worker thread must defer freeing the native rocksdb iterator until
// the read completes. _nextvAsync is the direct entry point that does not set
// abstract-level's kWorking flag, i.e. the path the abstract close() does not
// otherwise guard.

test('close() while an async iterator nextv is in flight', async function (t) {
  for (let i = 0; i < ITERATIONS; i++) {
    const db = testCommon.factory()
    await db.open()
    await seed(db, 200)
    const it = db.iterator()
    const p = it._nextvAsync(50, {})
    await Promise.all([db.close(), p.catch(() => {})])
  }
  t.pass('survived iterator-nextv+close')
  t.end()
})

// #2 (updates): closing while updates_next is in flight must defer the db
// teardown and the Database::Close() that resets the log iterator on a worker
// thread.

test('close() while an updates read is in flight', async function (t) {
  for (let i = 0; i < ITERATIONS; i++) {
    const db = testCommon.factory()
    await db.open()
    await seed(db, 3)
    const gen = db.updates()
    const np = gen.next()
    await Promise.all([db.close(), np.catch(() => {})])
    await gen.return().catch(() => {})
  }
  t.pass('survived updates+close')
  t.end()
})
