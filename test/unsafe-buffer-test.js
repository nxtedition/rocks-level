'use strict'

// Coverage for the zero-copy `unsafe: true` read path (util.h Convert ->
// napi_create_external_buffer backed by a heap PinnableSlice freed by a
// finalizer). It must return correct bytes and survive the backing slices being
// retained past the next read / GC.

const test = require('tape')
const testCommon = require('./common')

test('unsafe getMany returns correct values', async function (t) {
  const db = testCommon.factory({ keyEncoding: 'buffer', valueEncoding: 'buffer' })
  await db.open()

  const n = 256
  const batch = db.batch()
  const expected = []
  for (let i = 0; i < n; i++) {
    const key = Buffer.from('key' + String(i).padStart(4, '0'))
    const val = Buffer.allocUnsafe(64).fill(i & 0xff)
    expected.push(val)
    batch.put(key, val)
  }
  await batch.write()

  const keys = expected.map((_, i) => Buffer.from('key' + String(i).padStart(4, '0')))
  const safe = db._getManySync(keys, { valueEncoding: 'buffer' })
  const unsafe = db._getManySync(keys, { valueEncoding: 'buffer', unsafe: true })

  t.equal(unsafe.length, n, 'returns all values')
  let ok = true
  for (let i = 0; i < n; i++) {
    if (!unsafe[i].equals(expected[i]) || !unsafe[i].equals(safe[i])) ok = false
  }
  t.ok(ok, 'unsafe values match safe values and source bytes')

  // Retain the external buffers, force GC pressure, and re-read: the retained
  // buffers must still hold valid (pinned) bytes — i.e. no use-after-free.
  const retained = db._getManySync(keys, { valueEncoding: 'buffer', unsafe: true })
  for (let r = 0; r < 50; r++) db._getManySync(keys, { valueEncoding: 'buffer', unsafe: true })
  if (global.gc) global.gc()
  let stillValid = true
  for (let i = 0; i < n; i++) if (!retained[i].equals(expected[i])) stillValid = false
  t.ok(stillValid, 'retained unsafe buffers remain valid after further reads/GC')

  await db.close()
  t.end()
})

test('unsafe iterator nextv returns correct values', async function (t) {
  const db = testCommon.factory({ keyEncoding: 'buffer', valueEncoding: 'buffer' })
  await db.open()

  const batch = db.batch()
  const expected = new Map()
  for (let i = 0; i < 100; i++) {
    const key = Buffer.from('k' + String(i).padStart(3, '0'))
    const val = Buffer.allocUnsafe(32).fill(i & 0xff)
    expected.set(key.toString(), val)
    batch.put(key, val)
  }
  await batch.write()

  const entries = await db.iterator({ valueEncoding: 'buffer', unsafe: true }).all()
  t.equal(entries.length, 100, 'iterated all entries')
  let ok = true
  for (const [k, v] of entries) {
    if (!v.equals(expected.get(k.toString()))) ok = false
  }
  t.ok(ok, 'unsafe iterator values are correct')

  await db.close()
  t.end()
})

test('unsafe with empty values', async function (t) {
  const db = testCommon.factory({ keyEncoding: 'buffer', valueEncoding: 'buffer' })
  await db.open()
  await db.put(Buffer.from('empty'), Buffer.alloc(0))
  const [val] = db._getManySync([Buffer.from('empty')], { valueEncoding: 'buffer', unsafe: true })
  t.ok(Buffer.isBuffer(val), 'empty value returns a buffer')
  t.equal(val.length, 0, 'empty value has length 0')
  await db.close()
  t.end()
})
