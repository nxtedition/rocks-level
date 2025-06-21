const { RocksLevel } = require('../index')
const test = require('tape')
const fs = require('fs')
const path = require('path')

const dbPath = path.join(__dirname, 'testdb_compact')

function cleanup () {
  if (fs.existsSync(dbPath)) {
    fs.rmSync(dbPath, { recursive: true, force: true })
  }
}

test('compactRange', async (t) => {
  cleanup()
  const db = await RocksLevel.open(dbPath, { createIfMissing: true })
  await db.put('a', '1')
  await db.put('b', '2')
  await db.put('c', '3')

  /**
   * compactRange full range works correctly
   */
  await db.compactRange()
  t.pass('compactRange full range works correctly')

  /**
   * compactRange partial range works correctly
   */
  await db.compactRange({ start: 'a', end: 'b' })
  t.pass('compactRange partial range works correctly')

  await db.close()
  cleanup()
  t.end()
})
