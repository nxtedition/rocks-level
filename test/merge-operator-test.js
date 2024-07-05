'use strict'

const test = require('tape')
const testCommon = require('./common')

let db

test('setUp db', function (t) {
  db = testCommon.factory({
    valueEncoding: 'buffer',
    columns: {
      default: {
        mergeOperator: 'maxRev'
      }
    }
  })
  db.open(t.end.bind(t))
})

function makeVersion (str) {
  const buf = Buffer.from(str)
  return Buffer.concat([Buffer.from([buf.byteLength]), buf])
}

test('test merge maxRev()', async function (t) {
  const batch = db.batch()
  batch._merge('key1', makeVersion('1-asd'))
  batch._merge('key1', makeVersion('3-asd'))
  batch._merge('key1', makeVersion('2-asd'))
  await batch.write()

  t.same((await db.get('key1')).toString('utf-8', 1), '3-asd')

  t.end()
})

test('tearDown', async function (t) {
  await db.close()
  t.end()
})
