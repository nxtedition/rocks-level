'use strict'

const test = require('tape')
const testCommon = require('./common')

// Regression coverage for the iterator nextv `finished` / `limited` protocol and
// the byte/count cap accounting. The public `iterator.nextv()` drops these flags
// (abstract-level only forwards the entries), so we exercise the raw nxt API
// (_nextvSync / _nextvAsync) which returns { rows, finished, limited }.
//
// Invariants under test:
//   - count cap / highWaterMarkBytes cap  -> limited:true,  finished:false
//   - natural exhaustion (!Valid)         -> finished:true, limited:false
//   - user `limit` option reached         -> finished:true, limited:true
//   - paginating across caps loses/duplicates no rows, and caps stop BEFORE
//     advancing (no consumed-but-unemitted row).

async function put (db, keys) {
  const batch = db.batch()
  for (const k of keys) batch.put(k, 'V' + k)
  await batch.write()
}

function collectSync (it, size) {
  const all = []
  const batches = []
  while (true) {
    const { rows, finished, limited } = it._nextvSync(size, {})
    for (let n = 0; n < rows.length; n += 2) all.push(rows[n].toString())
    batches.push({ n: rows.length / 2, finished: !!finished, limited: !!limited })
    if (finished) break
  }
  return { all, batches }
}

async function collectAsync (it, size) {
  const all = []
  const batches = []
  while (true) {
    const { rows, finished, limited } = await it._nextvAsync(size, {})
    for (let n = 0; n < rows.length; n += 2) all.push(rows[n].toString())
    batches.push({ n: rows.length / 2, finished: !!finished, limited: !!limited })
    if (finished) break
  }
  return { all, batches }
}

test('nextv count cap reports limited (not finished) and paginates without loss', async function (t) {
  const keys = ['a', 'b', 'c', 'd', 'e']

  for (const [label, collect] of [['sync', collectSync], ['async', collectAsync]]) {
    const db = testCommon.factory()
    await db.open()
    await put(db, keys)

    const it = db.iterator()
    const { all, batches } = await collect(it, 2) // 2 pairs per batch
    await it.close()

    t.same(all, keys, `${label}: every key returned exactly once, in order`)

    const last = batches[batches.length - 1]
    t.ok(last.finished, `${label}: final batch is finished`)
    for (const b of batches.slice(0, -1)) {
      t.ok(b.limited && !b.finished, `${label}: capped batch is limited, not finished`)
    }

    await db.close()
  }

  t.end()
})

test('nextv highWaterMarkBytes cap paginates without loss and reports limited', async function (t) {
  const keys = []
  // Zero-padded so bytewise (RocksDB) order matches numeric order.
  for (let i = 0; i < 12; i++) keys.push('key' + String(i).padStart(2, '0'))

  for (const [label, collect] of [['sync', collectSync], ['async', collectAsync]]) {
    const db = testCommon.factory()
    await db.open()
    await put(db, keys)

    // Tiny HWM forces roughly one entry per batch, exercising many cap breaks.
    const it = db.iterator({ highWaterMarkBytes: 1 })
    const { all, batches } = await collect(it, 1e6)
    await it.close()

    t.same(all, keys, `${label}: HWM pagination returns all keys once, in order (no skip/dup)`)
    t.ok(batches.length > 1, `${label}: HWM actually split into multiple batches`)
    t.ok(batches[batches.length - 1].finished, `${label}: final batch finished`)
    t.ok(batches.slice(0, -1).every((b) => b.limited && !b.finished),
      `${label}: every non-final batch is limited, not finished`)

    await db.close()
  }

  t.end()
})

test('nextv reports finished:true + limited:true when the user limit is reached', async function (t) {
  for (const [label, nextv] of [
    ['sync', (it) => it._nextvSync(1e6, {})],
    ['async', (it) => it._nextvAsync(1e6, {})]
  ]) {
    const db = testCommon.factory()
    await db.open()
    await put(db, ['a', 'b', 'c', 'd', 'e'])

    const it = db.iterator({ limit: 2 })
    const { rows, finished, limited } = await nextv(it)
    await it.close()

    t.equal(rows.length, 4, `${label}: returns exactly limit entries (2 pairs)`)
    t.ok(finished, `${label}: limit is terminal (finished)`)
    t.ok(limited, `${label}: limit is flagged (limited)`)

    await db.close()
  }

  t.end()
})

test('nextv natural exhaustion reports finished:true and limited:false', async function (t) {
  for (const [label, nextv] of [
    ['sync', (it) => it._nextvSync(1e6, {})],
    ['async', (it) => it._nextvAsync(1e6, {})]
  ]) {
    const db = testCommon.factory()
    await db.open()
    await put(db, ['a', 'b', 'c'])

    const it = db.iterator() // no limit, default (huge) HWM
    const { rows, finished, limited } = await nextv(it)
    await it.close()

    t.equal(rows.length, 6, `${label}: all entries returned`)
    t.ok(finished, `${label}: exhausted iterator is finished`)
    t.notOk(limited, `${label}: exhaustion is not "limited"`)

    await db.close()
  }

  t.end()
})
