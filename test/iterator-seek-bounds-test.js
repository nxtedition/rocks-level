'use strict'

// Coverage for BaseIterator::Seek bound clamping (binding.cc): seeking outside
// the iterator's [gte/gt, lte/lt) window must clamp/invalidate correctly, for
// both forward and reverse iterators, and the `+ '\0'` boundary handling for
// gt/lte must be exact.

const test = require('tape')
const testCommon = require('./common')

async function seed (db) {
  const batch = db.batch()
  for (const k of ['b', 'c', 'd', 'e', 'f']) batch.put(k, 'V' + k)
  await batch.write()
}

test('seek past upper bound yields nothing (forward)', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const it = db.iterator({ gte: 'c', lt: 'e' })
  it.seek('z') // beyond upper bound
  const entry = await it.next()
  t.equal(entry, undefined, 'no entry after seeking past upper bound')
  await it.close()
  await db.close()
  t.end()
})

test('seek before lower bound yields nothing (abstract-level range contract)', async function (t) {
  // abstract-level mandates that seeking outside the range invalidates the
  // iterator rather than clamping (see its iterator-seek-test: gte:'5', seek '4'
  // -> undefined). This locks the binding's bound-clamp branch to that contract.
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const it = db.iterator({ gte: 'c', lt: 'e' })
  it.seek('a') // before lower bound
  const first = await it.next()
  t.equal(first, undefined, 'no entry after seeking before the lower bound')
  await it.close()
  await db.close()
  t.end()
})

test('seek within range positions exactly', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const it = db.iterator({ gte: 'b', lt: 'f' })
  it.seek('d')
  const entry = await it.next()
  t.equal(entry[0], 'd', 'seek lands on the exact key')
  await it.close()
  await db.close()
  t.end()
})

test('reverse seek past lower bound yields nothing', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const it = db.iterator({ gt: 'c', lte: 'e', reverse: true })
  it.seek('a') // below the (reverse) end
  const entry = await it.next()
  t.equal(entry, undefined, 'no entry after reverse-seeking past lower bound')
  await it.close()
  await db.close()
  t.end()
})

test('reverse seek within range positions at-or-before target', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const it = db.iterator({ reverse: true })
  it.seek('d')
  const entry = await it.next()
  t.equal(entry[0], 'd', 'reverse seek lands on the exact key when present')
  await it.close()
  await db.close()
  t.end()
})

test('gt boundary is exclusive, gte inclusive', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await seed(db)

  const gtEntries = await db.iterator({ gt: 'c', lt: 'e' }).all()
  t.same(gtEntries.map((e) => e[0]), ['d'], 'gt:c excludes c')

  const gteEntries = await db.iterator({ gte: 'c', lt: 'e' }).all()
  t.same(gteEntries.map((e) => e[0]), ['c', 'd'], 'gte:c includes c')

  const lteEntries = await db.iterator({ gte: 'c', lte: 'e' }).all()
  t.same(lteEntries.map((e) => e[0]), ['c', 'd', 'e'], 'lte:e includes e')

  await db.close()
  t.end()
})
