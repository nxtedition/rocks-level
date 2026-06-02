'use strict'

const test = require('tape')
const testCommon = require('./common')

// Regression for the uninitialized Encoding read in Iterator::create, reachable
// via querySync()/query(): when the options object omits keyEncoding/
// valueEncoding the native iterator used indeterminate enum values. The fix
// defaults both to Buffer (matching the other native read paths).
test('querySync without explicit encodings returns buffers and does not throw', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await db.put('a', 'A')
  await db.put('b', 'B')
  await db.put('c', 'C')

  let res
  t.doesNotThrow(() => {
    res = db.querySync({ gte: 'a', lte: 'c' })
  }, 'querySync without encodings does not throw')

  t.ok(res && Array.isArray(res.rows), 'returns a rows array')
  t.equal(res.rows.length, 6, 'three key/value pairs in a flat array')
  t.ok(Buffer.isBuffer(res.rows[0]), 'keys default to buffer encoding')
  t.ok(Buffer.isBuffer(res.rows[1]), 'values default to buffer encoding')
  t.equal(res.rows[0].toString(), 'a', 'first key matches')
  t.equal(res.rows[1].toString(), 'A', 'first value matches')

  await db.close()
  t.end()
})

test('query() callback form without encodings works', async function (t) {
  const db = testCommon.factory()
  await db.open()
  await db.put('x', '1')

  const res = await new Promise((resolve, reject) => {
    db.query({ gte: 'x', lte: 'x' }, (err, val) => err ? reject(err) : resolve(val))
  })

  t.ok(res && Array.isArray(res.rows), 'query() returns rows')
  t.equal(res.rows[0].toString(), 'x', 'key matches')

  await db.close()
  t.end()
})
