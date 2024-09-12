'use strict'

const binding = require('./binding')

class Regex {
  #context

  constructor (pattern) {
    this.#context = binding.regex_init(pattern)
  }

  test (buffer, byteOffset, byteLength) {
    if (byteOffset === undefined) {
      byteOffset = 0
    }

    if (byteLength === undefined) {
      byteLength = buffer.byteLength - byteOffset
    }

    return binding.regex_test(this.#context, buffer, byteOffset, byteLength)
  }

  testMany (values) {
    return binding.regex_test_many(this.#context, values)
  }
}

exports.Regex = Regex
