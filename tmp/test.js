'use strict'

const test = require('tape')
const testCommon = require('../test/common')

let db

test('setUp db', function (t) {
  db = testCommon.factory({
    walSizeLimit: 1e6
  })
  db.open(t.end.bind(t))
})

test('tearDown', async function (t) {
  await db.close()
  t.end()
})
