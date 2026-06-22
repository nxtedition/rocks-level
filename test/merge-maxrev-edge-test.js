'use strict'

// The durable `maxRev` merge operator (max_rev_operator.h compareRev) MUST select
// the same winner as the in-memory JS revision comparator (@nxtedition/util
// compareRev). RocksDB applies the operator during flush/compaction, so if the
// two disagree the stored "max revision" diverges from what the application
// believes is the max. This locks them together: leading-zero magnitude, the INF
// sentinel, id tiebreak, and zero-stripped length tiebreak.

const test = require('tape')
const testCommon = require('./common')

// Length-prefixed wire format: first byte = content length, then the revision.
function makeVersion (str) {
  const buf = Buffer.from(str)
  return Buffer.concat([Buffer.from([buf.byteLength]), buf])
}

// Reference: the canonical in-memory comparator (compare-rev.ts) over raw content.
function compareRevRef (a, b) {
  const I = 0x49; const ZERO = 0x30; const DASH = 0x2d
  if ((a[0] === I) !== (b[0] === I)) return a[0] === I ? 1 : -1
  const endA = a.length; let idxA = 0; let lenA = endA
  const endB = b.length; let idxB = 0; let lenB = endB
  while (a[idxA] === ZERO) { idxA++; lenA-- }
  while (b[idxB] === ZERO) { idxB++; lenB-- }
  let result = 0
  while (idxA < endA && idxB < endB) {
    const ac = a[idxA++]; const bc = b[idxB++]
    if (ac === DASH) { if (bc === DASH) break; return -1 } else if (bc === DASH) return 1
    result ||= ac - bc
  }
  if (result) return result
  while (idxA < endA && idxB < endB) { result = a[idxA++] - b[idxB++]; if (result) return result }
  return lenA - lenB
}

function jsMax (revs) {
  let max = revs[0]
  for (const r of revs) {
    if (compareRevRef(Buffer.from(max), Buffer.from(r)) < 0) max = r
  }
  return max
}

let db

test('maxRev edge setup', function (t) {
  db = testCommon.factory({
    valueEncoding: 'buffer',
    columns: { default: { mergeOperator: 'maxRev' } }
  })
  db.open(t.end.bind(t))
})

test('maxRev: higher numeric revision wins regardless of leading zeros', async function (t) {
  const cases = [
    [['01-x', '2-x'], '2-x'], // 2 > 1 even though "01" sorts lexically before "2"
    [['9-x', '12-x'], '12-x'], // more significant digits = larger
    [['99-z', '100-z'], '100-z'],
    [['005-a', '5-b'], '5-b'], // equal magnitude (5): id 'b' > 'a'
    [['010-y', '9-z', '1-x'], '010-y'] // 10 > 9 > 1
  ]
  for (const [ops, winner] of cases) {
    const key = Buffer.from('lz:' + ops.join(','))
    const b = db.batch()
    for (const o of ops) b._merge(key, makeVersion(o))
    await b.write()
    t.same((await db.get(key)).toString('utf8', 1), winner, `${ops} -> ${winner}`)
  }
  t.end()
})

test('maxRev: INF sentinel outranks every numeric revision', async function (t) {
  for (const [ops, winner] of [
    [['999999-x', 'INF-a'], 'INF-a'],
    [['INF-a', '5-b'], 'INF-a'],
    [['INF-a', 'INF-b'], 'INF-b'] // two INF -> id tiebreak
  ]) {
    const key = Buffer.from('inf:' + ops.join(','))
    const b = db.batch()
    for (const o of ops) b._merge(key, makeVersion(o))
    await b.write()
    t.same((await db.get(key)).toString('utf8', 1), winner, `${ops} -> ${winner}`)
  }
  t.end()
})

test('maxRev: high bytes (>=0x80) compare as unsigned', async function (t) {
  const key = Buffer.from('highbyte')
  const b = db.batch()
  b._merge(key, makeVersion(Buffer.from([0x31, 0x2d, 0x10]))) // "1-\x10"
  b._merge(key, makeVersion(Buffer.from([0x31, 0x2d, 0x80]))) // "1-\x80" (larger unsigned)
  await b.write()
  const got = await db.get(key)
  t.same([...got.subarray(1)], [0x31, 0x2d, 0x80], '0x80 > 0x10 unsigned')
  t.end()
})

test('maxRev: durable winner == JS-comparator max over random revisions', async function (t) {
  // Property test: random revisions (with leading zeros / INF) merged in random
  // order must yield the same max the in-memory comparator would compute.
  let mismatches = 0
  for (let i = 0; i < 400; i++) {
    const n = 2 + (i % 6)
    const revs = []
    for (let j = 0; j < n; j++) {
      const zeros = '0'.repeat(i % 3)
      const num = (i * 7 + j * 13) % 50 === 0 ? 'INF' : zeros + ((i * 7 + j * 13) % 200)
      revs.push(`${num}-${String.fromCharCode(0x61 + (j % 6))}`)
    }
    const key = Buffer.from('prop:' + i)
    const b = db.batch()
    for (const r of revs) b._merge(key, makeVersion(r))
    await b.write()
    const got = (await db.get(key)).toString('utf8', 1)
    const want = jsMax(revs)
    // Equal-magnitude+id revisions are indistinguishable; compare by the JS
    // comparator rather than by exact bytes.
    if (compareRevRef(Buffer.from(got), Buffer.from(want)) !== 0) {
      mismatches++
      if (mismatches <= 3) t.comment(`mismatch: got=${got} want=${want} from ${revs}`)
    }
  }
  t.equal(mismatches, 0, 'durable maxRev agrees with JS comparator on all random sets')
  t.end()
})

test('maxRev edge teardown', async function (t) {
  await db.close()
  t.end()
})
