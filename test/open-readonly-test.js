'use strict'

const make = require('./make')
const { RocksLevel } = require('..')

make('open database in read-only mode', function (db, t, done) {
  const batch = db.batch()
  batch.put('a', 1)
  batch.put('b', 2)
  batch.put('c', 3)
  batch.write(function (err) {
    t.ifError(err, 'no error from batch()')
    db.close(function (err) {
      t.ifError(err, 'no error from close()')
      const readOnlyDb = new RocksLevel(db.location)
      readOnlyDb.openForReadOnly(function (err) {
        console.log('@', err)
        t.ifError(err, 'no error from openForReadOnly()')
        console.log('@@', err)
        readOnlyDb.get('a', function (err, value) {
          console.log('@@@', err)
          console.log('@@@', err)
          t.ifError(err, 'no error from get()')
          console.log('@@@ value]>', value)
          t.equal(value, '1', 'can read value')
        })
        readOnlyDb.close(done)
      })
    })
  })
})
