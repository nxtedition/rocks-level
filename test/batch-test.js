'use strict'

const test = require('tape')
const testCommon = require('./common')

let db

test('setUp db', function (t) {
  db = testCommon.factory({
    walSizeLimit: 1e6
  })
  db.open(t.end.bind(t))
})

// test('test batch([]) (array-form) emits batch event', async function (t) {
//   t.plan(2)

//   const db = testCommon.factory()
//   await db.open()

//   t.ok(db.supports.events.batch)

//   db.on('batch', function (ops) {
//     t.same(ops, [{ type: 'put', key: 456, value: 99, custom: 123 }])
//   })

//   await db.batch([{ type: 'put', key: 456, value: 99, custom: 123 }])
//   await db.close()
// })

// test('test batch iterate', async function (t) {
//   const batch = db.batch()
//   batch._put('1', 'a')
//   batch._put('2', 'b')
//   t.same([...batch], [
//     { type: 'put', key: '1', value: 'a' },
//     { type: 'put', key: '2', value: 'b' }
//   ])
// })

test('tearDown', async function (t) {
  await db.close()
  t.end()
})
