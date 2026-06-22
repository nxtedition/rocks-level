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

test('test identity()', async function (t) {
  t.ok(typeof db.identity === 'string')
})

test('tearDown', async function (t) {
  await db.close()
  t.end()
})
