#pragma once

#include <cstddef>
#include <rocksdb/slice.h>
#include <rocksdb/merge_operator.h>

#include <iostream>

// Compares two length-prefixed revision operands and returns <0, 0, >0.
//
// This MUST stay byte-for-byte order-compatible with the in-memory JS comparator
// (@nxtedition/util compareRev, lib/packages/util/src/compare-rev.ts), because
// RocksDB selects the durable winner with this operator while the application
// compares the same revisions in memory with the JS one — if they disagree, the
// stored "max revision" diverges from what the app believes is the max. A 500k
// randomized fuzz (leading zeros, INF, length ties, missing dashes) confirms
// parity. Revisions are `<number>-<id>` (e.g. `12-7a00`, `INF-…`) compared as:
//   1. INF sentinel: a number beginning with 'I' is +infinity (largest).
//   2. leading zeros are skipped so `01-x` == `1-x` in magnitude.
//   3. the number is compared digit-by-digit, terminated by '-'; the side whose
//      number ends first (fewer significant digits) is smaller.
//   4. then the id is compared bytewise; finally the zero-stripped length breaks
//      ties (a longer number = larger revision).
int compareRev(const rocksdb::Slice& a, const rocksdb::Slice& b) {
  if (a.empty()) {
    return b.empty() ? 0 : -1;
  } else if (b.empty()) {
    return 1;
  }

  // The first byte is a length prefix declaring the content length. Clamp it to
  // the bytes actually available (size - 1) so malformed/truncated operands can
  // never over-read, and cast through unsigned char so a prefix >= 0x80 is not
  // sign-extended. endA/endB are exclusive end offsets: content is at [1, endX).
  std::size_t indexA = 1;
  std::size_t indexB = 1;
  const std::size_t endA = 1 + std::min<std::size_t>(static_cast<unsigned char>(a[0]), a.size() - 1);
  const std::size_t endB = 1 + std::min<std::size_t>(static_cast<unsigned char>(b[0]), b.size() - 1);

  // INF-XXXX sorts above every numeric revision. Mirror the JS comparator's
  // explicit sentinel rather than relying on 'I' (0x49) happening to exceed the
  // digit bytes.
  const bool infA = indexA < endA && a[indexA] == 'I';
  const bool infB = indexB < endB && b[indexB] == 'I';
  if (infA != infB) {
    return infA ? 1 : -1;
  }

  // Skip leading zeroes, tracking the zero-stripped content length for the final
  // tiebreak, so `01-x` and `1-x` compare as the same magnitude.
  std::size_t lenA = endA - indexA;
  std::size_t lenB = endB - indexB;
  while (indexA < endA && a[indexA] == '0') {
    ++indexA;
    --lenA;
  }
  while (indexB < endB && b[indexB] == '0') {
    ++indexB;
    --lenB;
  }

  // Compare the revision number. Compare bytes as unsigned char: rocksdb::Slice
  // operator[] returns (signed-on-most-platforms) char, so a byte >= 0x80 would
  // otherwise sort as negative and order opposite to the JS comparator, which
  // reads bytes as unsigned (Buffer[i] in 0..255). Keeping both sides unsigned
  // ensures the in-memory ordering and this durable maxRev merge agree.
  auto result = 0;
  while (indexA < endA && indexB < endB) {
    const unsigned char ac = static_cast<unsigned char>(a[indexA++]);
    const unsigned char bc = static_cast<unsigned char>(b[indexB++]);

    if (ac == '-') {
      if (bc == '-') {
        break;
      }
      return -1;
    } else if (bc == '-') {
      return 1;
    }

    if (!result) {
      result = ac == bc ? 0 : ac < bc ? -1 : 1;
    }
  }

  if (result) {
    return result;
  }

  // Compare the rest (unsigned, for the same reason as the loop above).
  while (indexA < endA && indexB < endB) {
    const unsigned char ac = static_cast<unsigned char>(a[indexA++]);
    const unsigned char bc = static_cast<unsigned char>(b[indexB++]);
    if (ac != bc) {
      return ac < bc ? -1 : 1;
    }
  }

  return static_cast<int>(lenA) - static_cast<int>(lenB);
}

class MaxRevOperator : public rocksdb::MergeOperator {
 public:
  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override {
    rocksdb::Slice& max = merge_out->existing_operand;
    if (merge_in.existing_value) {
      max = rocksdb::Slice(merge_in.existing_value->data(),
                  merge_in.existing_value->size());
    } else if (max.data() == nullptr) {
      max = rocksdb::Slice();
    }

    for (const auto& op : merge_in.operand_list) {
      if (compareRev(max, op) < 0) {
        max = op;
      }
    }

    return true;
  }

  bool PartialMerge(const rocksdb::Slice& /*key*/, const rocksdb::Slice& left_operand,
                    const rocksdb::Slice& right_operand, std::string* new_value,
                    rocksdb::Logger* /*logger*/) const override {
    if (compareRev(left_operand, right_operand) >= 0) {
      new_value->assign(left_operand.data(), left_operand.size());
    } else {
      new_value->assign(right_operand.data(), right_operand.size());
    }
    return true;
  }

  bool PartialMergeMulti(const rocksdb::Slice& /*key*/,
                         const std::deque<rocksdb::Slice>& operand_list,
                         std::string* new_value,
                         rocksdb::Logger* /*logger*/) const override {
    rocksdb::Slice max;
    for (const auto& operand : operand_list) {
      if (compareRev(max, operand) < 0) {
        max = operand;
      }
    }

    new_value->assign(max.data(), max.size());
    return true;
  }

  static const char* kClassName() { return "MaxRevOperator"; }
  static const char* kNickName() { return "maxRev"; }
  const char* Name() const override { return kClassName(); }
  const char* NickName() const override { return kNickName(); }
};
