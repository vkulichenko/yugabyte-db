// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef YB_UTIL_BIT_STREAM_UTILS_INLINE_H
#define YB_UTIL_BIT_STREAM_UTILS_INLINE_H

#include <algorithm>

#include "yb/util/bit-stream-utils.h"
#include "yb/util/alignment.h"

namespace yb {

inline void BitWriter::PutValue(uint64_t v, int num_bits) {
  // TODO: revisit this limit if necessary (can be raised to 64 by fixing some edge cases)
  DCHECK_LE(num_bits, 32);
  DCHECK_EQ(v >> num_bits, 0) << "v = " << v << ", num_bits = " << num_bits;

  buffered_values_ |= v << bit_offset_;
  bit_offset_ += num_bits;

  if (PREDICT_FALSE(bit_offset_ >= 64)) {
    // Flush buffered_values_ and write out bits of v that did not fit
    buffer_->reserve(align_up(byte_offset_ + 8, 8));
    buffer_->resize(byte_offset_ + 8);
    DCHECK_LE(byte_offset_ + 8, buffer_->capacity());
    memcpy(buffer_->data() + byte_offset_, &buffered_values_, 8);
    buffered_values_ = 0;
    byte_offset_ += 8;
    bit_offset_ -= 64;
    buffered_values_ = v >> (num_bits - bit_offset_);
  }
  DCHECK_LT(bit_offset_, 64);
}

inline void BitWriter::Flush(bool align) {
  int num_bytes = ceil_div(bit_offset_, 8);
  buffer_->reserve(align_up(byte_offset_ + num_bytes, 8));
  buffer_->resize(byte_offset_ + num_bytes);
  DCHECK_LE(byte_offset_ + num_bytes, buffer_->capacity());
  memcpy(buffer_->data() + byte_offset_, &buffered_values_, num_bytes);

  if (align) {
    buffered_values_ = 0;
    byte_offset_ += num_bytes;
    bit_offset_ = 0;
  }
}

inline uint8_t* BitWriter::GetNextBytePtr(int num_bytes) {
  Flush(/* align */ true);
  buffer_->reserve(align_up(byte_offset_ + num_bytes, 8));
  buffer_->resize(byte_offset_ + num_bytes);
  uint8_t* ptr = buffer_->data() + byte_offset_;
  byte_offset_ += num_bytes;
  DCHECK_LE(byte_offset_, buffer_->capacity());
  return ptr;
}

template<typename T>
inline void BitWriter::PutAligned(T val, int num_bytes) {
  DCHECK_LE(num_bytes, sizeof(T));
  uint8_t* ptr = GetNextBytePtr(num_bytes);
  memcpy(ptr, &val, num_bytes);
}

inline void BitWriter::PutVlqInt(int32_t v) {
  while ((v & 0xFFFFFF80) != 0L) {
    PutAligned<uint8_t>((v & 0x7F) | 0x80, 1);
    v >>= 7;
  }
  PutAligned<uint8_t>(v & 0x7F, 1);
}


inline BitReader::BitReader(const uint8_t* buffer, size_t buffer_len)
  : buffer_(buffer),
    max_bytes_(buffer_len),
    buffered_values_(0),
    byte_offset_(0),
    bit_offset_(0) {
  auto num_bytes = std::min<size_t>(8, max_bytes_);
  memcpy(&buffered_values_, buffer_ + byte_offset_, num_bytes);
}

inline void BitReader::BufferValues() {
  auto bytes_remaining = max_bytes_ - byte_offset_;
  if (PREDICT_TRUE(bytes_remaining >= 8)) {
    memcpy(&buffered_values_, buffer_ + byte_offset_, 8);
  } else {
    memcpy(&buffered_values_, buffer_ + byte_offset_, bytes_remaining);
  }
}

template<typename T>
inline bool BitReader::GetValue(int num_bits, T* v) {
  // TODO: revisit this limit if necessary
  DCHECK_LE(num_bits, 32);
  DCHECK_LE(num_bits, sizeof(T) * 8);

  if (PREDICT_FALSE(byte_offset_ * 8 + bit_offset_ + num_bits > max_bytes_ * 8)) return false;

  *v = static_cast<T>(
      BitUtil::TrailingBits(buffered_values_, bit_offset_ + num_bits) >> bit_offset_);

  bit_offset_ += num_bits;
  if (bit_offset_ >= 64) {
    byte_offset_ += 8;
    bit_offset_ -= 64;
    BufferValues();
    // Read bits of v that crossed into new buffered_values_
    *v |= static_cast<T>(
        BitUtil::TrailingBits(buffered_values_, bit_offset_) << (num_bits - bit_offset_));
  }
  DCHECK_LE(bit_offset_, 64);
  return true;
}

inline void BitReader::Rewind(ssize_t num_bits) {
  bit_offset_ -= num_bits;
  if (bit_offset_ >= 0) {
    return;
  }
  while (bit_offset_ < 0) {
    auto seek_back = std::min<size_t>(byte_offset_, 8);
    byte_offset_ -= seek_back;
    bit_offset_ += seek_back * 8;
  }
  // This should only be executed *if* rewinding by 'num_bits'
  // make the existing buffered_values_ invalid
  DCHECK_GE(byte_offset_, 0); // Check for underflow
  memcpy(&buffered_values_, buffer_ + byte_offset_, 8);
}

inline void BitReader::SeekToBit(uint stream_position) {
  DCHECK_LE(stream_position, max_bytes_ * 8);

  auto delta = stream_position - position();
  if (delta == 0) {
    return;
  } else if (delta < 0) {
    Rewind(position() - stream_position);
  } else {
    bit_offset_ += delta;
    while (bit_offset_ >= 64) {
      byte_offset_ +=8;
      bit_offset_ -= 64;
      if (bit_offset_ < 64) {
        // This should only be executed if seeking to
        // 'stream_position' makes the existing buffered_values_
        // invalid.
        BufferValues();
      }
    }
  }
}

template<typename T>
inline bool BitReader::GetAligned(int num_bytes, T* v) {
  DCHECK_LE(num_bytes, sizeof(T));
  auto bytes_read = ceil_div<size_t>(bit_offset_, 8);
  if (PREDICT_FALSE(byte_offset_ + bytes_read + num_bytes > max_bytes_)) return false;

  // Advance byte_offset to next unread byte and read num_bytes
  byte_offset_ += bytes_read;
  memcpy(v, buffer_ + byte_offset_, num_bytes);
  byte_offset_ += num_bytes;

  // Reset buffered_values_
  bit_offset_ = 0;
  auto bytes_remaining = max_bytes_ - byte_offset_;
  if (PREDICT_TRUE(bytes_remaining >= 8)) {
    memcpy(&buffered_values_, buffer_ + byte_offset_, 8);
  } else {
    memcpy(&buffered_values_, buffer_ + byte_offset_, bytes_remaining);
  }
  return true;
}

inline bool BitReader::GetVlqInt(int32_t* v) {
  *v = 0;
  int shift = 0;
  int num_bytes = 0;
  uint8_t byte = 0;
  do {
    if (!GetAligned<uint8_t>(1, &byte)) return false;
    *v |= (byte & 0x7F) << shift;
    shift += 7;
    DCHECK_LE(++num_bytes, MAX_VLQ_BYTE_LEN);
  } while ((byte & 0x80) != 0);
  return true;
}

} // namespace yb

#endif // YB_UTIL_BIT_STREAM_UTILS_INLINE_H
