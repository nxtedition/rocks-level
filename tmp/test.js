'use strict'

const tempy = require('tempy')
const { RocksLevel } = require('@nxtedition/rocksdb')

const db = new RocksLevel(tempy.directory(), {})
db.open().then(() => db.close())
