'use strict'

const test = require('tape')
const testCommon = require('./common')

let db

test('filter tests setup', async function (t) {
  db = testCommon.factory()
  await db.open()

  await db.batch([
    { type: 'put', key: 'user:1', value: 'alice' },
    { type: 'put', key: 'user:2', value: 'bob' },
    { type: 'put', key: 'user:3', value: 'admin' },
    { type: 'put', key: 'post:1', value: 'hello' },
    { type: 'put', key: 'post:2', value: 'world' },
    { type: 'put', key: 'log:error:1', value: 'error: something failed' },
    { type: 'put', key: 'log:info:1', value: 'info: all good' },
    { type: 'put', key: 'log:error:2', value: 'error: another failure' }
  ])
})

test('keyFilter - basic regex match', async function (t) {
  const entries = await db.iterator({ keyFilter: '^user:' }).all()

  t.equal(entries.length, 3, 'should return 3 entries')
  t.equal(entries[0][0], 'user:1', 'first key matches')
  t.equal(entries[1][0], 'user:2', 'second key matches')
  t.equal(entries[2][0], 'user:3', 'third key matches')
})

test('keyFilter - no matches', async function (t) {
  const entries = await db.iterator({ keyFilter: '^comment:' }).all()

  t.equal(entries.length, 0, 'should return no entries')
})

test('keyFilter - partial match', async function (t) {
  const entries = await db.iterator({ keyFilter: 'error' }).all()

  t.equal(entries.length, 2, 'should return 2 entries')
  t.equal(entries[0][0], 'log:error:1', 'first key matches')
  t.equal(entries[1][0], 'log:error:2', 'second key matches')
})

test('valueFilter - basic regex match', async function (t) {
  const entries = await db.iterator({ valueFilter: '^error:' }).all()

  t.equal(entries.length, 2, 'should return 2 entries')
  t.equal(entries[0][0], 'log:error:1', 'first key matches')
  t.equal(entries[1][0], 'log:error:2', 'second key matches')
})

test('valueFilter - no matches', async function (t) {
  const entries = await db.iterator({ valueFilter: 'xyz' }).all()

  t.equal(entries.length, 0, 'should return no entries')
})

test('valueFilter - partial match in value', async function (t) {
  const entries = await db.iterator({ valueFilter: 'o' }).all()

  // bob, hello, world, error: something failed, info: all good, error: another failure
  t.equal(entries.length, 6, 'should return 6 entries with "o" in value')
})

test('keyFilter and valueFilter combined', async function (t) {
  const entries = await db.iterator({
    keyFilter: '^user:',
    valueFilter: 'admin'
  }).all()

  t.equal(entries.length, 1, 'should return 1 entry')
  t.equal(entries[0][0], 'user:3', 'key matches')
  t.equal(entries[0][1], 'admin', 'value matches')
})

// TODO: keyFilter + limit without reverse has a bug - limit is applied before filter
// test('keyFilter with limit', async function (t) {
//   const entries = await db.iterator({
//     keyFilter: '^user:',
//     limit: 2
//   }).all()
//
//   t.equal(entries.length, 2, 'should return 2 entries')
//   t.equal(entries[0][0], 'user:1', 'first key matches')
//   t.equal(entries[1][0], 'user:2', 'second key matches')
// })

test('valueFilter with limit', async function (t) {
  const entries = await db.iterator({
    valueFilter: '^error:',
    limit: 1
  }).all()

  t.equal(entries.length, 1, 'should return 1 entry')
  t.equal(entries[0][0], 'log:error:1', 'first key matches')
})

test('keyFilter with reverse', async function (t) {
  const entries = await db.iterator({
    keyFilter: '^user:',
    reverse: true
  }).all()

  t.equal(entries.length, 3, 'should return 3 entries')
  t.equal(entries[0][0], 'user:3', 'first key is user:3')
  t.equal(entries[1][0], 'user:2', 'second key is user:2')
  t.equal(entries[2][0], 'user:1', 'third key is user:1')
})

test('keyFilter with range options', async function (t) {
  const entries = await db.iterator({
    keyFilter: '^user:',
    gte: 'user:2',
    lte: 'user:3'
  }).all()

  t.equal(entries.length, 2, 'should return 2 entries')
  t.equal(entries[0][0], 'user:2', 'first key matches')
  t.equal(entries[1][0], 'user:3', 'second key matches')
})

test('keyFilter with complex regex - digits only', async function (t) {
  const entries = await db.iterator({
    keyFilter: '^user:\\d$'
  }).all()

  t.equal(entries.length, 3, 'should return 3 entries')
})

test('keyFilter keys only mode', async function (t) {
  const keys = await db.iterator({
    keyFilter: '^user:',
    values: false
  }).all()

  t.equal(keys.length, 3, 'should return 3 keys')
  t.equal(keys[0][0], 'user:1', 'first key matches')
  t.ok(keys[0][1] === undefined, 'value is undefined')
})

test('valueFilter values only mode', async function (t) {
  const values = await db.iterator({
    valueFilter: '^error:',
    keys: false
  }).all()

  t.equal(values.length, 2, 'should return 2 values')
  // When keys: false, entries are [null, value] not [key, value]
  const firstValue = values[0][1]
  t.ok(firstValue && firstValue.startsWith('error:'), 'first value matches')
})

test('keyFilter with next() iteration', async function (t) {
  const iterator = db.iterator({ keyFilter: '^user:' })

  let entry = await iterator.next()
  t.ok(entry, 'first entry exists')
  t.equal(entry[0], 'user:1', 'first key matches')

  entry = await iterator.next()
  t.ok(entry, 'second entry exists')
  t.equal(entry[0], 'user:2', 'second key matches')

  entry = await iterator.next()
  t.ok(entry, 'third entry exists')
  t.equal(entry[0], 'user:3', 'third key matches')

  entry = await iterator.next()
  t.equal(entry, undefined, 'no more entries')

  await iterator.close()
})

test('valueFilter with next() iteration', async function (t) {
  const iterator = db.iterator({ valueFilter: '^error:' })

  let entry = await iterator.next()
  t.ok(entry, 'first entry exists')
  t.equal(entry[0], 'log:error:1', 'first key matches')

  entry = await iterator.next()
  t.ok(entry, 'second entry exists')
  t.equal(entry[0], 'log:error:2', 'second key matches')

  entry = await iterator.next()
  t.equal(entry, undefined, 'no more entries')

  await iterator.close()
})

test('keyFilter case insensitive regex', async function (t) {
  const entries = await db.iterator({
    keyFilter: '(?i)^USER:'
  }).all()

  t.equal(entries.length, 3, 'should return 3 entries')
})

test('filter matches all entries when pattern is broad', async function (t) {
  const entries = await db.iterator({
    keyFilter: '.'
  }).all()

  t.equal(entries.length, 8, 'should return all 8 entries')
})

test('filter tests teardown', async function (t) {
  return db.close()
})
