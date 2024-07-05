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

test('max rev', async function (t) {
  const batch = db._chainedBatch()

  const rev1 = '03-SuJnevw6qTIF1P-SO34j9xEGiJbQq-render'
  const rec1 = makeVersion(rev1)
  rec1[0] = rec1.byteLength - 1
  batch._merge('test', rec1, {
    column: db.columns.default
  })
  const rev2 = '02-SuJnevw6qTIF1P-SO34j9xEGiJbQq-render213123213'
  const rec2 = Buffer.from(rev2)
  rec2[0] = rec2.byteLength - 1
  batch._merge('test', rec2, {
    column: db.columns.default
  })

  await batch.write()

  t.same((await db.get('test')).toString('utf8', 1), rev1)

  t.end()
})

test('tearDown', async function (t) {
  await db.close()
  t.end()
})
