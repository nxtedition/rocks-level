'use strict'

const test = require('tape')
const testCommon = require('./common')

// Regression for the native null-deref: reading db.sequence / db.identity
// before open or after close dereferenced a null DB pointer and crashed the
// process. They must now throw a catchable LEVEL_DATABASE_NOT_OPEN error.
test('sequence/identity throw (not crash) when the db is not open', async function (t) {
  const db = testCommon.factory()

  // Right after construction the db is still opening (not yet 'open').
  t.throws(() => db.sequence, /not open/i, 'sequence throws before open')
  t.throws(() => db.identity, /not open/i, 'identity throws before open')

  await db.open()
  t.equal(typeof db.sequence, 'number', 'sequence returns a number when open')
  t.equal(typeof db.identity, 'string', 'identity returns a string when open')

  await db.close()
  t.throws(() => db.sequence, /not open/i, 'sequence throws after close')
  t.throws(() => db.identity, /not open/i, 'identity throws after close')

  t.end()
})

function grabError (fn) {
  try {
    fn()
    return {}
  } catch (err) {
    return err
  }
}

test('sequence/identity report the LEVEL_DATABASE_NOT_OPEN code', function (t) {
  const db = testCommon.factory()

  t.equal(grabError(() => db.sequence).code, 'LEVEL_DATABASE_NOT_OPEN', 'sequence error code')
  t.equal(grabError(() => db.identity).code, 'LEVEL_DATABASE_NOT_OPEN', 'identity error code')

  t.end()
})
