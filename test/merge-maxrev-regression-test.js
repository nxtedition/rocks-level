'use strict'

const test = require('tape')
const testCommon = require('./common')

function makeVersion (str) {
  const buf = Buffer.from(str)
  return Buffer.concat([Buffer.from([buf.byteLength]), buf])
}

function makeVersionRaw (contentBytes) {
  const buf = Buffer.from(contentBytes)
  return Buffer.concat([Buffer.from([buf.byteLength]), buf])
}

function mergeFactory () {
  return testCommon.factory({
    valueEncoding: 'buffer',
    columns: {
      default: {
        mergeOperator: 'maxRev'
      }
    }
  })
}

// Regression for the compareRev off-by-one: the final content byte was never
// compared, so two revisions differing only in their last byte compared equal
// and the strictly-larger one was silently not adopted by the maxRev merge.
test('maxRev adopts the larger revision when only the last byte differs', async function (t) {
  const db = mergeFactory()
  await db.open()

  const lo = makeVersion('3-aaa')
  const hi = makeVersion('3-aab')

  const b1 = db.batch()
  b1._merge('fwd', lo)
  b1._merge('fwd', hi)
  await b1.write()
  t.equal((await db.get('fwd')).toString('utf8', 1), '3-aab', 'lower then higher -> higher wins')

  const b2 = db.batch()
  b2._merge('rev', hi)
  b2._merge('rev', lo)
  await b2.write()
  t.equal((await db.get('rev')).toString('utf8', 1), '3-aab', 'higher then lower -> higher wins')

  await db.close()
  t.end()
})

test('maxRev compares the revision number before the rest', async function (t) {
  const db = mergeFactory()
  await db.open()

  const b = db.batch()
  b._merge('k', makeVersion('1-zzz'))
  b._merge('k', makeVersion('3-aaa'))
  b._merge('k', makeVersion('2-mmm'))
  await b.write()
  t.equal((await db.get('k')).toString('utf8', 1), '3-aaa', 'highest revision number wins')

  await db.close()
  t.end()
})

// Regression for the malformed/short-input hardening (unsigned-char prefix and
// clamping to the available bytes): zero-content and oversized-prefix operands
// must not over-read or crash.
test('maxRev handles zero-content and oversized-prefix operands safely', async function (t) {
  const db = mergeFactory()
  await db.open()

  // Zero declared content reliably loses to a real value.
  const b = db.batch()
  b._merge('k', Buffer.from([0])) // declares 0 content bytes
  b._merge('k', makeVersion('1-aaa'))
  await b.write()
  t.equal((await db.get('k')).toString('utf8', 1), '1-aaa', 'real value beats zero-content operand')

  // A length prefix far larger than the buffer must be clamped to the available
  // bytes (no over-read / crash). The winner is data-dependent, so we only
  // assert the merge + read complete and return a buffer.
  const b2 = db.batch()
  b2._merge('m', Buffer.from([200, 0x41, 0x42])) // prefix (200) far exceeds size
  b2._merge('m', makeVersion('9-zzz'))
  await b2.write()
  t.ok(Buffer.isBuffer(await db.get('m')), 'oversized-prefix merge does not crash')

  await db.close()
  t.end()
})

// Regression for the signed-char comparison: rocksdb::Slice::operator[] returns
// a (signed) char, so a content byte >= 0x80 used to sort as negative and order
// opposite to the JS comparator, which reads bytes as unsigned (0..255). With
// the same revision number, the operand whose post-'-' byte is 0x80 must beat
// the one whose byte is 0x7f (128 > 127 unsigned); the old signed code picked
// 0x7f instead, diverging from the in-memory ordering.
test('maxRev orders high bytes (>= 0x80) as unsigned, matching the JS comparator', async function (t) {
  const db = mergeFactory()
  await db.open()

  const hi = makeVersionRaw([0x33, 0x2d, 0x80]) // "3-" + 0x80
  const lo = makeVersionRaw([0x33, 0x2d, 0x7f]) // "3-" + 0x7f

  const b1 = db.batch()
  b1._merge('fwd', lo)
  b1._merge('fwd', hi)
  await b1.write()
  t.deepEqual(
    [...(await db.get('fwd')).subarray(1)],
    [0x33, 0x2d, 0x80],
    'lower then higher -> 0x80 byte wins'
  )

  const b2 = db.batch()
  b2._merge('rev', hi)
  b2._merge('rev', lo)
  await b2.write()
  t.deepEqual(
    [...(await db.get('rev')).subarray(1)],
    [0x33, 0x2d, 0x80],
    'higher then lower -> 0x80 byte wins'
  )

  await db.close()
  t.end()
})
