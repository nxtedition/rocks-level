'use strict'

const make = require('./make')

make('updates yields put updates', async function (db, t, done) {
  const updates = []

  for await (const update of db.updates()) {
    updates.push(update)
  }

  t.ok(updates.length > 0, 'has updates')

  const last = updates[updates.length - 1]
  t.ok(last.seq > 0, 'has sequence number')
  t.ok(Array.isArray(last.rows), 'rows is an array')

  done()
})

make('updates with since option skips earlier updates', function (db, t, done) {
  db.put('four', '4', async function (err) {
    t.ifError(err, 'no error from put()')

    const allUpdates = []
    for await (const update of db.updates()) {
      allUpdates.push(update)
    }

    // The last update should be our put of 'four'
    const last = allUpdates[allUpdates.length - 1]
    const sinceUpdates = []
    for await (const update of db.updates({ since: last.seq })) {
      sinceUpdates.push(update)
    }

    t.equal(sinceUpdates.length, 1, 'has exactly one update since last seq')
    t.equal(sinceUpdates[0].seq, last.seq, 'seq matches')

    const rows = sinceUpdates[0].rows
    t.equal(rows[0], 'put', 'operation is put')
    t.equal(rows[1], 'four', 'key matches')
    t.equal(rows[2], '4', 'value matches')

    done()
  })
})

make('updates with del operations', function (db, t, done) {
  db.del('one', async function (err) {
    t.ifError(err, 'no error from del()')

    const allUpdates = []
    for await (const update of db.updates()) {
      allUpdates.push(update)
    }

    const last = allUpdates[allUpdates.length - 1]
    const rows = last.rows
    t.equal(rows[0], 'del', 'operation is del')
    t.equal(rows[1], 'one', 'key matches')

    done()
  })
})

make('updates with batch operations', function (db, t, done) {
  db.batch([
    { type: 'put', key: 'x', value: '24' },
    { type: 'put', key: 'y', value: '25' },
    { type: 'del', key: 'one' }
  ], async function (err) {
    t.ifError(err, 'no error from batch()')

    const allUpdates = []
    for await (const update of db.updates()) {
      allUpdates.push(update)
    }

    const last = allUpdates[allUpdates.length - 1]
    const rows = last.rows
    // rows is a flat array with stride 4: [op, key, value, column, ...]
    t.equal(rows[0], 'put', 'first op is put')
    t.equal(rows[1], 'x', 'first key matches')
    t.equal(rows[2], '24', 'first value matches')

    t.equal(rows[4], 'put', 'second op is put')
    t.equal(rows[5], 'y', 'second key matches')
    t.equal(rows[6], '25', 'second value matches')

    t.equal(rows[8], 'del', 'third op is del')
    t.equal(rows[9], 'one', 'third key matches')

    done()
  })
})

make('updates since:0 returns all updates', async function (db, t, done) {
  const updates = []

  for await (const update of db.updates({ since: 0 })) {
    updates.push(update)
  }

  t.ok(updates.length > 0, 'has updates from the beginning')

  // Should contain the initial batch with one, two, three
  let found = false
  for await (const update of updates) {
    for (let i = 0; i < update.rows.length; i += 4) {
      if (update.rows[i] === 'put' && update.rows[i + 1] === 'one') {
        found = true
      }
    }
  }

  t.ok(found, 'found initial put for key "one"')

  done()
})
