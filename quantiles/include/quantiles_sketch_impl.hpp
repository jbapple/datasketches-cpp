/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef _QUANTILES_SKETCH_IMPL_HPP_
#define _QUANTILES_SKETCH_IMPL_HPP_

#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iomanip>

#include "common_defs.hpp"
#include "count_zeros.hpp"
#include "quantiles_sketch.hpp"

namespace datasketches {

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::quantiles_sketch(uint16_t k, const A& allocator):
allocator_(allocator),
k_(k),
n_(0),
bit_pattern_(0),
base_buffer_(allocator_),
levels_(allocator_),
min_value_(nullptr),
max_value_(nullptr),
is_sorted_(true)
{
  if (k < quantiles_constants::MIN_K || k > quantiles_constants::MAX_K) {
    throw std::invalid_argument("K must be >= " + std::to_string(quantiles_constants::MIN_K) + " and <= " + std::to_string(quantiles_constants::MAX_K) + ": " + std::to_string(k));
  }
  base_buffer_.reserve(2 * std::min(quantiles_constants::MIN_K, k));
}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::quantiles_sketch(const quantiles_sketch& other):
allocator_(other.allocator_),
k_(other.k_),
n_(other.n_),
bit_pattern_(other.bit_pattern_),
base_buffer_(other.base_buffer_),
levels_(other.levels_),
min_value_(nullptr),
max_value_(nullptr),
is_sorted_(other.is_sorted_)
{
  if (other.min_value_ != nullptr) min_value_ = new (allocator_.allocate(1)) T(*other.min_value_);
  if (other.max_value_ != nullptr) max_value_ = new (allocator_.allocate(1)) T(*other.max_value_);
}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::quantiles_sketch(quantiles_sketch&& other) noexcept:
allocator_(other.allocator_),
k_(other.k_),
n_(other.n_),
bit_pattern_(other.bit_pattern_),
base_buffer_(std::move(other.base_buffer_)),
levels_(std::move(other.levels_)),
min_value_(other.min_value_),
max_value_(other.max_value_),
is_sorted_(other.is_sorted_)
{
  other.min_value_ = nullptr;
  other.max_value_ = nullptr;
}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>& quantiles_sketch<T, C, A>::operator=(const quantiles_sketch& other) {
  quantiles_sketch<T, C, A> copy(other);
  std::swap(allocator_, copy.allocator_);
  std::swap(k_, copy.k_);
  std::swap(n_, copy.n_);
  std::swap(bit_pattern_, copy.bit_pattern_);
  std::swap(base_buffer_, copy.base_buffer_);
  std::swap(levels_, copy.levels_);
  std::swap(min_value_, copy.min_value_);
  std::swap(max_value_, copy.max_value_);
  std::swap(is_sorted_, copy.is_sorted_);
  return *this;
}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>& quantiles_sketch<T, C, A>::operator=(quantiles_sketch&& other) noexcept {
  std::swap(allocator_, other.allocator_);
  std::swap(k_, other.k_);
  std::swap(n_, other.n_);
  std::swap(bit_pattern_, other.bit_pattern_);
  std::swap(base_buffer_, other.base_buffer_);
  std::swap(levels_, other.levels_);
  std::swap(min_value_, other.min_value_);
  std::swap(max_value_, other.max_value_);
  std::swap(is_sorted_, other.is_sorted_);
  return *this;
}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::quantiles_sketch(uint16_t k, uint64_t n, uint64_t bit_pattern,
      Level&& base_buffer, VectorLevels&& levels,
      std::unique_ptr<T, item_deleter> min_value, std::unique_ptr<T, item_deleter> max_value,
      bool is_sorted, const A& allocator) :
allocator_(allocator),
k_(k),
n_(n),
bit_pattern_(bit_pattern),
base_buffer_(std::move(base_buffer)),
levels_(std::move(levels)),
min_value_(min_value.release()),
max_value_(max_value.release()),
is_sorted_(is_sorted)
{}

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::~quantiles_sketch() {
  if (min_value_ != nullptr) {
    min_value_->~T();
    allocator_.deallocate(min_value_, 1);
  }
  if (max_value_ != nullptr) {
    max_value_->~T();
    allocator_.deallocate(max_value_, 1);
  }
}

template<typename T, typename C, typename A>
template<typename FwdT>
void quantiles_sketch<T, C, A>::update(FwdT&& item) {
  if (!check_update_value(item)) { return; }
  if (is_empty()) {
    min_value_ = new (allocator_.allocate(1)) T(item);
    max_value_ = new (allocator_.allocate(1)) T(item);
  } else {
    if (C()(item, *min_value_)) *min_value_ = item;
    if (C()(*max_value_, item)) *max_value_ = item;
  }

  // if exceed capacity, grow until size 2k -- assumes eager processing
  if (base_buffer_.size() + 1 > base_buffer_.capacity())
    grow_base_buffer();

  base_buffer_.push_back(std::forward<FwdT>(item));
  ++n_;

  if (base_buffer_.size() > 1)
    is_sorted_ = false;
  
  if (base_buffer_.size() == 2 * k_)
    process_full_base_buffer();
}

template<typename T, typename C, typename A>
template<typename SerDe>
void quantiles_sketch<T, C, A>::serialize(std::ostream& os, const SerDe& serde) const {
  const uint8_t preamble_longs = is_empty() ? PREAMBLE_LONGS_SHORT : PREAMBLE_LONGS_FULL;
  write(os, preamble_longs);
  const uint8_t ser_ver = SERIAL_VERSION;
  write(os, ser_ver);
  const uint8_t family = FAMILY;
  write(os, family);

  // empty, ordered, compact are valid flags
  const uint8_t flags_byte(
      (is_empty() ? 1 << flags::IS_EMPTY : 0)
    | (is_sorted_ ? 1 << flags::IS_SORTED : 0)
    | (1 << flags::IS_COMPACT) // always compact -- could be optional for numeric types?
  );
  write(os, flags_byte);
  write(os, k_);
  uint16_t unused = 0;
  write(os, unused);

  if (!is_empty()) {
    write(os, n_);

    // min and max
    serde.serialize(os, min_value_, 1);
    serde.serialize(os, max_value_, 1);

    // base buffer items
    serde.serialize(os, base_buffer_.data(), base_buffer_.size());

    // levels, only when data is present
    for (Level lvl : levels_) {
      if (lvl.size() > 0)
        serde.serialize(os, lvl.data(), lvl.size());
    }
  }
}

template<typename T, typename C, typename A>
template<typename SerDe>
auto quantiles_sketch<T, C, A>::serialize(unsigned header_size_bytes, const SerDe& serde) const -> vector_bytes {
  const size_t size = get_serialized_size_bytes() + header_size_bytes;
  vector_bytes bytes(size, 0, allocator_);
  uint8_t* ptr = bytes.data() + header_size_bytes;
  const uint8_t* end_ptr = ptr + size;
  
  const uint8_t preamble_longs = is_empty() ? PREAMBLE_LONGS_SHORT : PREAMBLE_LONGS_FULL;
  ptr += copy_to_mem(preamble_longs, ptr);
  const uint8_t ser_ver = SERIAL_VERSION;
  ptr += copy_to_mem(ser_ver, ptr);
  const uint8_t family = FAMILY;
  ptr += copy_to_mem(family, ptr);

  // empty, ordered, compact are valid flags
  const uint8_t flags_byte(
      (is_empty() ? 1 << flags::IS_EMPTY : 0)
    | (is_sorted_ ? 1 << flags::IS_SORTED : 0)
    | (1 << flags::IS_COMPACT) // always compact -- could be optional for numeric types?
  );
  ptr += copy_to_mem(flags_byte, ptr);
  ptr += copy_to_mem(k_, ptr);
  ptr += sizeof(uint16_t); // 2 unused bytes
  
  if (!is_empty()) {

    ptr += copy_to_mem(n_, ptr);
    
    // min and max
    ptr += serde.serialize(ptr, end_ptr - ptr, min_value_, 1);
    ptr += serde.serialize(ptr, end_ptr - ptr, max_value_, 1);
 
    // base buffer items
    if (base_buffer_.size() > 0)
      ptr += serde.serialize(ptr, end_ptr - ptr, base_buffer_.data(), base_buffer_.size());
    
    // levels, only when data is present
    for (Level lvl : levels_) {
      if (lvl.size() > 0)
        ptr += serde.serialize(ptr, end_ptr - ptr, lvl.data(), lvl.size());
    }
  }

  return bytes;
}

template<typename T, typename C, typename A>
template<typename SerDe>
auto quantiles_sketch<T, C, A>::deserialize(std::istream &is, const SerDe& serde, const A &allocator) -> quantiles_sketch {
  const auto preamble_longs = read<uint8_t>(is);
  const auto serial_version = read<uint8_t>(is);
  const auto family_id = read<uint8_t>(is);
  const auto flags_byte = read<uint8_t>(is);
  const auto k = read<uint16_t>(is);
  read<uint16_t>(is); // unused

  check_serial_version(serial_version); // a little redundant with the next line, but explicit checks
  check_family_id(family_id);
  check_header_validity(preamble_longs, flags_byte, serial_version);

  if (!is.good()) throw std::runtime_error("error reading from std::istream");
  const bool is_empty = (flags_byte & (1 << flags::IS_EMPTY)) > 0;
  if (is_empty) {
    return quantiles_sketch(k, allocator);
  }

  const auto items_seen = read<uint64_t>(is);

  const bool is_compact = (flags_byte & (1 << flags::IS_COMPACT)) > 0;
  const bool is_sorted = (flags_byte & (1 << flags::IS_SORTED)) > 0;

  A alloc(allocator);
  auto item_buffer_deleter = [&alloc](T* ptr) { alloc.deallocate(ptr, 1); };
  std::unique_ptr<T, decltype(item_buffer_deleter)> min_value_buffer(alloc.allocate(1), item_buffer_deleter);
  std::unique_ptr<T, decltype(item_buffer_deleter)> max_value_buffer(alloc.allocate(1), item_buffer_deleter);
  std::unique_ptr<T, item_deleter> min_value(nullptr, item_deleter(allocator));
  std::unique_ptr<T, item_deleter> max_value(nullptr, item_deleter(allocator));

  serde.deserialize(is, min_value_buffer.get(), 1);
  // serde call did not throw, repackage with destrtuctor
  min_value = std::unique_ptr<T, item_deleter>(min_value_buffer.release(), item_deleter(allocator));
  serde.deserialize(is, max_value_buffer.get(), 1);
  // serde call did not throw, repackage with destrtuctor
  max_value = std::unique_ptr<T, item_deleter>(max_value_buffer.release(), item_deleter(allocator));

  // allocate buffers as needed
  const uint8_t levels_needed = compute_levels_needed(k, items_seen);
  const uint64_t bit_pattern = compute_bit_pattern(k, items_seen);

  // Java provides a compact storage layout for a sketch of primitive doubles. The C++ version
  // does not currently operate sketches in compact mode, but will only serialize as compact
  // to avoid complications around serialization of empty values for generic type T. We also need
  // to be able to ingest either serialized format from Java.

  // load base buffer
  const uint32_t bb_items = compute_base_buffer_items(k, items_seen);
  uint32_t items_to_read = is_compact ? bb_items : 2 * k;
  Level base_buffer = deserialize_array(is, items_to_read, 2 * k, serde, allocator);
  
  // populate vector of Levels directly
  VectorLevels levels(allocator);
  levels.reserve(levels_needed);
  if (levels_needed > 0) {
    uint64_t working_pattern = bit_pattern;
    for (size_t i = 0; i < levels_needed; ++i, working_pattern >>= 1) {
     
      if ((working_pattern & 0x01) == 1) {
        Level level = deserialize_array(is, k, k, serde, allocator);
        levels.push_back(std::move(level));
      } else {
        Level level(allocator);
        level.reserve(k);
        levels.push_back(std::move(level));
      }
    }
  }

  return quantiles_sketch(k, items_seen, bit_pattern,
    std::move(base_buffer), std::move(levels), std::move(min_value), std::move(max_value), is_sorted, allocator);
}

template<typename T, typename C, typename A>
template<typename SerDe>
auto quantiles_sketch<T, C, A>::deserialize_array(std::istream& is, uint32_t num_items, uint32_t capacity, const SerDe& serde, const A& allocator) -> Level {
  A alloc(allocator);
  std::unique_ptr<T, items_deleter> items(alloc.allocate(num_items), items_deleter(allocator, false, num_items));
  serde.deserialize(is, items.get(), num_items);
  // serde did not throw, enable destructors
  items.get_deleter().set_destroy(true);
  if (!is.good()) throw std::runtime_error("error reading from std::istream");

  // succesfully read, now put into a Level
  Level level(allocator);
  level.reserve(capacity);
  level.insert(level.begin(),
               std::make_move_iterator(items.get()),
               std::make_move_iterator(items.get() + num_items));
  return std::move(level);
}

template<typename T, typename C, typename A>
template<typename SerDe>
auto quantiles_sketch<T, C, A>::deserialize(const void* bytes, size_t size, const SerDe& serde, const A &allocator) -> quantiles_sketch {
  ensure_minimum_memory(size, 8);
  const char* ptr = static_cast<const char*>(bytes);
  const char* end_ptr = static_cast<const char*>(bytes) + size;

  uint8_t preamble_longs;
  ptr += copy_from_mem(ptr, preamble_longs);
  uint8_t serial_version;
  ptr += copy_from_mem(ptr, serial_version);
  uint8_t family_id;
  ptr += copy_from_mem(ptr, family_id);
  uint8_t flags_byte;
  ptr += copy_from_mem(ptr, flags_byte);
  uint16_t k;
  ptr += copy_from_mem(ptr, k);
  uint16_t unused;
  ptr += copy_from_mem(ptr, unused);

  check_serial_version(serial_version); // a little redundant with the next line, but explicit checks
  check_family_id(family_id);
  check_header_validity(preamble_longs, flags_byte, serial_version);

  const bool is_empty = (flags_byte & (1 << flags::IS_EMPTY)) > 0;
  if (is_empty) {
    return quantiles_sketch(k, allocator);
  }

  ensure_minimum_memory(size, 16);
  uint64_t items_seen;
  ptr += copy_from_mem(ptr, items_seen);

  const bool is_compact = (flags_byte & (1 << flags::IS_COMPACT)) > 0;
  const bool is_sorted = (flags_byte & (1 << flags::IS_SORTED)) > 0;

  A alloc(allocator);
  auto item_buffer_deleter = [&alloc](T* ptr) { alloc.deallocate(ptr, 1); };
  std::unique_ptr<T, decltype(item_buffer_deleter)> min_value_buffer(alloc.allocate(1), item_buffer_deleter);
  std::unique_ptr<T, decltype(item_buffer_deleter)> max_value_buffer(alloc.allocate(1), item_buffer_deleter);
  std::unique_ptr<T, item_deleter> min_value(nullptr, item_deleter(allocator));
  std::unique_ptr<T, item_deleter> max_value(nullptr, item_deleter(allocator));

  ptr += serde.deserialize(ptr, end_ptr - ptr, min_value_buffer.get(), 1);
  // serde call did not throw, repackage with destrtuctor
  min_value = std::unique_ptr<T, item_deleter>(min_value_buffer.release(), item_deleter(allocator));
  ptr += serde.deserialize(ptr, end_ptr - ptr, max_value_buffer.get(), 1);
  // serde call did not throw, repackage with destrtuctor
  max_value = std::unique_ptr<T, item_deleter>(max_value_buffer.release(), item_deleter(allocator));

  // allocate buffers as needed
  const uint8_t levels_needed = compute_levels_needed(k, items_seen);
  const uint64_t bit_pattern = compute_bit_pattern(k, items_seen);

  // Java provides a compact storage layout for a sketch of primitive doubles. The C++ version
  // does not currently operate sketches in compact mode, but will only serialize as compact
  // to avoid complications around serialization of empty values for generic type T. We also need
  // to be able to ingest either serialized format from Java.

  // load base buffer
  const uint32_t bb_items = compute_base_buffer_items(k, items_seen);
  uint32_t items_to_read = is_compact ? bb_items : 2 * k;
  auto base_buffer_pair = deserialize_array(ptr, end_ptr - ptr, items_to_read, 2 * k, serde, allocator);
  ptr += base_buffer_pair.second;
  
  // populate vector of Levels directly
  VectorLevels levels(allocator);
  levels.reserve(levels_needed);
  if (levels_needed > 0) {
    uint64_t working_pattern = bit_pattern;
    for (size_t i = 0; i < levels_needed; ++i, working_pattern >>= 1) {
     
      if ((working_pattern & 0x01) == 1) {
        auto pair = deserialize_array(ptr, end_ptr - ptr, k, k, serde, allocator);
        ptr += pair.second;
        levels.push_back(std::move(pair.first));
      } else {
        Level level(allocator);
        level.reserve(k);
        levels.push_back(std::move(level));
      }
    }
  }

  return quantiles_sketch(k, items_seen, bit_pattern,
    std::move(base_buffer_pair.first), std::move(levels), std::move(min_value), std::move(max_value), is_sorted, allocator);
}

template<typename T, typename C, typename A>
template<typename SerDe>
auto quantiles_sketch<T, C, A>::deserialize_array(const void* bytes, size_t size, uint32_t num_items, uint32_t capacity, const SerDe& serde, const A& allocator)
  -> std::pair<Level, size_t> {
  const char* ptr = static_cast<const char*>(bytes);
  const char* end_ptr = static_cast<const char*>(bytes) + size;
  A alloc(allocator);
  std::unique_ptr<T, items_deleter> items(alloc.allocate(num_items), items_deleter(allocator, false, num_items));
  ptr += serde.deserialize(ptr, end_ptr - ptr, items.get(), num_items);
  // serde did not throw, enable destructors
  items.get_deleter().set_destroy(true);
  
  // succesfully read, now put into a Level
  Level level(allocator);
  level.reserve(capacity);
  level.insert(level.begin(),
               std::make_move_iterator(items.get()),
               std::make_move_iterator(items.get() + num_items));
  
  return std::pair<Level, size_t>(std::move(level), ptr - static_cast<const char*>(bytes));
}

template<typename T, typename C, typename A>
string<A> quantiles_sketch<T, C, A>::to_string(bool print_levels, bool print_items) const {
  // Using a temporary stream for implementation here does not comply with AllocatorAwareContainer requirements.
  // The stream does not support passing an allocator instance, and alternatives are complicated.
  std::ostringstream os;
  os << "### Quantiles Sketch summary:" << std::endl;
  os << "   K              : " << k_ << std::endl;
  os << "   N              : " << n_ << std::endl;
  os << "   Epsilon        : " << std::setprecision(3) << get_normalized_rank_error(false) * 100 << "%" << std::endl;
  os << "   Epsilon PMF    : " << get_normalized_rank_error(true) * 100 << "%" << std::endl;
  os << "   Empty          : " << (is_empty() ? "true" : "false") << std::endl;
  os << "   Estimation mode: " << (is_estimation_mode() ? "true" : "false") << std::endl;
  os << "   Levels (w/o BB): " << levels_.size() << std::endl;
  os << "   Used Levels    : " << compute_valid_levels(bit_pattern_) << std::endl;
  os << "   Retained items : " << get_num_retained() << std::endl;
  os << "   Storage bytes  : " << get_serialized_size_bytes() << std::endl;
  if (!is_empty()) {
    os << "   Min value      : " << *min_value_ << std::endl;
    os << "   Max value      : " << *max_value_ << std::endl;
  }
  os << "### End sketch summary" << std::endl;

  if (print_levels) {
    os << "### Quantiles Sketch levels:" << std::endl;
    os << "   index: items in use" << std::endl;
    os << "   BB: " << base_buffer_.size() << std::endl;
    for (uint8_t i = 0; i < levels_.size(); i++) {
      os << "   " << static_cast<unsigned int>(i) << ": " << levels_[i].size() << std::endl;
    }
    os << "### End sketch levels" << std::endl;
  }

  if (print_items) {
    os << "### Quantiles Sketch data:" << std::endl;
    uint8_t level = 0;
    os << " BB:" << std::endl;
    for (const T& item : base_buffer_) {
      os << "    " << std::to_string(item) << std::endl;
    }
    for (uint8_t i = 0; i < levels_.size(); ++i) {
      os << " level " << static_cast<unsigned int>(level) << ":" << std::endl;
      for (const T& item : levels_[i]) {
        os << "   " << std::to_string(item) << std::endl;
      }
    }
    os << "### End sketch data" << std::endl;
  }
  return string<A>(os.str().c_str(), allocator_);
}

template<typename T, typename C, typename A>
uint16_t quantiles_sketch<T, C, A>::get_k() const {
  return k_;
}

template<typename T, typename C, typename A>
uint64_t quantiles_sketch<T, C, A>::get_n() const {
  return n_;
}

template<typename T, typename C, typename A>
bool quantiles_sketch<T, C, A>::is_empty() const {
  return n_ == 0;
}

template<typename T, typename C, typename A>
bool quantiles_sketch<T, C, A>::is_estimation_mode() const {
  return bit_pattern_ != 0;
}

template<typename T, typename C, typename A>
uint32_t quantiles_sketch<T, C, A>::get_num_retained() const {
  return compute_retained_items(k_, n_);
}

template<typename T, typename C, typename A>
const T& quantiles_sketch<T, C, A>::get_min_value() const {
  if (is_empty()) return get_invalid_value();
  return *min_value_;
}

template<typename T, typename C, typename A>
const T& quantiles_sketch<T, C, A>::get_max_value() const {
  if (is_empty()) return get_invalid_value();
  return *max_value_;
}

// implementation for fixed-size arithmetic types (integral and floating point)
template<typename T, typename C, typename A>
template<typename TT, typename std::enable_if<std::is_arithmetic<TT>::value, int>::type>
size_t quantiles_sketch<T, C, A>::get_serialized_size_bytes() const {
  if (is_empty()) { return EMPTY_SIZE_BYTES; }
  return DATA_START + ((get_num_retained() + 2) * sizeof(TT));
}

// implementation for all other types
template<typename T, typename C, typename A>
template<typename SerDe, typename TT, typename std::enable_if<!std::is_arithmetic<TT>::value, int>::type>
size_t quantiles_sketch<T, C, A>::get_serialized_size_bytes(const SerDe& serde) const {
  if (is_empty()) { return EMPTY_SIZE_BYTES; }
  size_t size = DATA_START;
  size += serde.size_of_item(*min_value_);
  size += serde.size_of_item(*max_value_);
  for (auto it: *this) size += serde.size_of_item(it.first);
  return size;
}

template<typename T, typename C, typename A>
double quantiles_sketch<T, C, A>::get_normalized_rank_error(bool is_pmf) const {
  return get_normalized_rank_error(k_, is_pmf);
}

template<typename T, typename C, typename A>
double quantiles_sketch<T, C, A>::get_normalized_rank_error(uint16_t k, bool is_pmf) {
  return is_pmf
      ? 1.854 / std::pow(k, 0.9657)
      : 1.576 / std::pow(k, 0.9726);
}

template<typename T, typename C, typename A>
class quantiles_sketch<T, C, A>::calculator_deleter {
  public:
  explicit calculator_deleter(const AllocCalc& allocator): allocator_(allocator) {}
  void operator() (QuantileCalculator* ptr) {
    if (ptr != nullptr) {
      ptr->~QuantileCalculator();
      allocator_.deallocate(ptr, 1);
    }
  }
  private:
  AllocCalc allocator_;
};

template<typename T, typename C, typename A>
template<bool inclusive>
auto quantiles_sketch<T, C, A>::get_quantile_calculator() const -> QuantileCalculatorPtr {
  // allow side effect of sorting the base buffer
  // can't set the sorted flag since this is a const method
  if (!is_sorted_) {
    std::sort(const_cast<Level&>(base_buffer_).begin(), const_cast<Level&>(base_buffer_).end(), C());
  }

  AllocCalc ac(allocator_);
  QuantileCalculatorPtr quantile_calculator_ptr(
    new (ac.allocate(1)) quantile_calculator<T, C, A>(n_, ac),
    calculator_deleter(ac)
  );

  uint64_t lg_weight = 0;
  quantile_calculator_ptr->add(base_buffer_.data(), base_buffer_.data() + base_buffer_.size(), lg_weight);
  for (auto& level : levels_) {
    ++lg_weight;
    if (level.empty()) { continue; }
    quantile_calculator_ptr->add(level.data(), level.data() + k_, lg_weight);
  }
  quantile_calculator_ptr->template convert_to_cummulative<inclusive>();
  return quantile_calculator_ptr;
}

template<typename T, typename C, typename A>
template<bool inclusive>
const T& quantiles_sketch<T, C, A>::get_quantile(double rank) const {
  if (is_empty()) return get_invalid_value();
  if (rank == 0.0) return *min_value_;
  if (rank == 1.0) return *max_value_;
  if ((rank < 0.0) || (rank > 1.0)) {
    throw std::invalid_argument("Rank cannot be less than zero or greater than 1.0");
  }
  return *(get_quantile_calculator<inclusive>()->get_quantile(rank));
}

template<typename T, typename C, typename A>
template<bool inclusive>
std::vector<T, A> quantiles_sketch<T, C, A>::get_quantiles(const double* ranks, uint32_t size) const {
  std::vector<T, A> quantiles(allocator_);
  if (is_empty()) return quantiles;
  QuantileCalculatorPtr quantile_calculator_ptr(nullptr, calculator_deleter(allocator_));
  quantiles.reserve(size);
  for (uint32_t i = 0; i < size; ++i) {
    const double rank = ranks[i];
    if ((rank < 0.0) || (rank > 1.0)) {
      throw std::invalid_argument("rank cannot be less than zero or greater than 1.0");
    }
    if      (rank == 0.0) quantiles.push_back(*min_value_);
    else if (rank == 1.0) quantiles.push_back(*max_value_);
    else {
      if (!quantile_calculator_ptr) {
        // has side effect of sorting level zero if needed
        quantile_calculator_ptr = const_cast<quantiles_sketch*>(this)->get_quantile_calculator<inclusive>();
      }
      quantiles.push_back(*(quantile_calculator_ptr->get_quantile(rank)));
    }
  }
  return quantiles;
}

template<typename T, typename C, typename A>
template<bool inclusive>
std::vector<T, A> quantiles_sketch<T, C, A>::get_quantiles(uint32_t num) const {
  if (is_empty()) return std::vector<T, A>(allocator_);
  if (num == 0) {
    throw std::invalid_argument("num must be > 0");
  }
  vector_double fractions(num, 0, allocator_);
  fractions[0] = 0.0;
  for (size_t i = 1; i < num; i++) {
    fractions[i] = static_cast<double>(i) / (num - 1);
  }
  if (num > 1) {
    fractions[num - 1] = 1.0;
  }
  return get_quantiles<inclusive>(fractions.data(), num);
}

template<typename T, typename C, typename A>
template<bool inclusive>
double quantiles_sketch<T, C, A>::get_rank(const T& value) const {
  if (is_empty()) return std::numeric_limits<double>::quiet_NaN();
  uint64_t weight = 1;
  uint64_t total = 0;
  for (const T &item: base_buffer_) {
    if (inclusive ? !C()(value, item) : C()(item, value))
      total += weight;
  }

  weight *= 2;
  for (uint8_t level = 0; level < levels_.size(); ++level, weight *= 2) {
    if (levels_[level].empty()) { continue; }
    const T* data = levels_[level].data();
    for (uint16_t i = 0; i < k_; ++i) {
      if (inclusive ? !C()(value, data[i]) : C()(data[i], value))
        total += weight;
      else
        break;  // levels are sorted, no point comparing further
    }
  }
  return (double) total / n_;
}

template<typename T, typename C, typename A>
template<bool inclusive>
auto quantiles_sketch<T, C, A>::get_PMF(const T* split_points, uint32_t size) const -> vector_double {
  auto buckets = get_CDF<inclusive>(split_points, size);
  if (is_empty()) return buckets;
  for (uint32_t i = size; i > 0; --i) {
    buckets[i] -= buckets[i - 1];
  }
  return buckets;
}

template<typename T, typename C, typename A>
template<bool inclusive>
auto quantiles_sketch<T, C, A>::get_CDF(const T* split_points, uint32_t size) const -> vector_double {
  vector_double buckets(allocator_);
  if (is_empty()) return buckets;
  check_split_points(split_points, size);
  buckets.reserve(size + 1);
  for (uint32_t i = 0; i < size; ++i) buckets.push_back(get_rank<inclusive>(split_points[i]));
  buckets.push_back(1);
  return buckets;
}

template<typename T, typename C, typename A>
uint32_t quantiles_sketch<T, C, A>::compute_retained_items(const uint16_t k, const uint64_t n) {
  uint32_t bb_count = compute_base_buffer_items(k, n);
  uint64_t bit_pattern = compute_bit_pattern(k, n);
  uint32_t valid_levels = compute_valid_levels(bit_pattern);
  return bb_count + (k * valid_levels);
}

template<typename T, typename C, typename A>
uint32_t quantiles_sketch<T, C, A>::compute_base_buffer_items(const uint16_t k, const uint64_t n) {
  return n % (static_cast<uint64_t>(2) * k);
}

template<typename T, typename C, typename A>
uint64_t quantiles_sketch<T, C, A>::compute_bit_pattern(const uint16_t k, const uint64_t n) {
  return n / (static_cast<uint64_t>(2) * k);
}

template<typename T, typename C, typename A>
uint32_t quantiles_sketch<T, C, A>::compute_valid_levels(const uint64_t bit_pattern) {
  // TODO: Java's Long.bitCount() probably uses a better method
  uint64_t bp = bit_pattern;
  uint32_t count = 0;
  while (bp > 0) {
    if ((bp & 0x01) == 1) ++count;
    bp >>= 1;
  }
  return count;
}

template<typename T, typename C, typename A>
uint8_t quantiles_sketch<T, C, A>::compute_levels_needed(const uint16_t k, const uint64_t n) {
  return static_cast<uint8_t>(64U) - count_leading_zeros_in_u64(n / (2 * k));
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::check_serial_version(uint8_t serial_version) {
  if (serial_version == SERIAL_VERSION || serial_version == SERIAL_VERSION_1 || serial_version == SERIAL_VERSION_2)
    return;
  else
    throw std::invalid_argument("Possible corruption. Unrecognized serialization version: " + std::to_string(serial_version));
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::check_family_id(uint8_t family_id) {
  if (family_id == FAMILY)
    return;
  else
    throw std::invalid_argument("Possible corruption. Family id does not indicate quantiles sketch: " + std::to_string(family_id));
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::check_header_validity(uint8_t preamble_longs, uint8_t flags_byte, uint8_t serial_version) {
  bool empty = (flags_byte & (1 << flags::IS_EMPTY)) > 0;
  bool compact = (flags_byte & (1 << flags::IS_COMPACT)) > 0;

  uint8_t sw = (compact ? 1 : 0) + (2 * (empty ? 1 : 0))
               + (4 * (serial_version & 0xF)) + (32 * (preamble_longs & 0x3F));
  bool valid = true;
  
  switch (sw) { // exhaustive list and description of all valid cases
      case 38  : break; //!compact,  empty, serVer = 1, preLongs = 1; always stored as not compact
      case 164 : break; //!compact, !empty, serVer = 1, preLongs = 5; always stored as not compact
      case 42  : break; //!compact,  empty, serVer = 2, preLongs = 1; always stored as compact
      case 72  : break; //!compact, !empty, serVer = 2, preLongs = 2; always stored as compact
      case 47  : break; // compact,  empty, serVer = 3, preLongs = 1;
      case 46  : break; //!compact,  empty, serVer = 3, preLongs = 1;
      case 79  : break; // compact,  empty, serVer = 3, preLongs = 2;
      case 78  : break; //!compact,  empty, serVer = 3, preLongs = 2;
      case 77  : break; // compact, !empty, serVer = 3, preLongs = 2;
      case 76  : break; //!compact, !empty, serVer = 3, preLongs = 2;
      default : //all other case values are invalid
        valid = false;
  }

  if (!valid) {
    std::ostringstream os;
    os << "Possible sketch corruption. Inconsistent state: "
       << "preamble_longs = " << preamble_longs
       << ", empty = " << (empty ? "true" : "false")
       << ", serialization_version = " << serial_version
       << ", compact = " << (compact ? "true" : "false");
    throw std::invalid_argument(os.str());
  }
}

template <typename T, typename C, typename A>
typename quantiles_sketch<T, C, A>::const_iterator quantiles_sketch<T, C, A>::begin() const {
  return quantiles_sketch<T, C, A>::const_iterator(base_buffer_, levels_, k_, n_, false);
}

template <typename T, typename C, typename A>
typename quantiles_sketch<T, C, A>::const_iterator quantiles_sketch<T, C, A>::end() const {
  return quantiles_sketch<T, C, A>::const_iterator(base_buffer_, levels_, k_, n_, true);
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::grow_base_buffer() {
  size_t new_size = std::max(std::min(static_cast<size_t>(2 * k_), 2 * base_buffer_.size()), static_cast<size_t>(1));
  base_buffer_.reserve(new_size);
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::process_full_base_buffer() {
  // make sure there will be enough levels for the propagation
  grow_levels_if_needed(); // note: n_ was already incremented by update() before this

  std::sort(base_buffer_.begin(), base_buffer_.end(), C());
  in_place_propagate_carry(0,
                           levels_[0], // unused here, but 0 is guaranteed to exist
                           base_buffer_,
                           true, *this);
  base_buffer_.clear();
  is_sorted_ = true;
  assert(n_ / (2 * k_) == bit_pattern_);  // internal consistency check
}

template<typename T, typename C, typename A>
bool quantiles_sketch<T, C, A>::grow_levels_if_needed() {
  uint8_t levels_needed = compute_levels_needed(k_, n_);
  if (levels_needed == 0)
    return false; // don't need levels and might have small base buffer. Possible during merges.

  // from here on, assume full size base buffer (2k) and at least one additional level
  if (levels_needed <= levels_.size())
    return false;

  Level empty_level(allocator_);
  empty_level.reserve(k_);
  levels_.push_back(std::move(empty_level));
  return true;
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::in_place_propagate_carry(uint8_t starting_level,
                                                            Level& buf_size_k, Level& buf_size_2k, 
                                                            bool apply_as_update,
                                                            quantiles_sketch<T,C,A>& sketch) {
  const uint64_t bit_pattern = sketch.bit_pattern_;
  const int k = sketch.k_;

  uint8_t ending_level = lowest_zero_bit_starting_at(bit_pattern, starting_level);

  if (apply_as_update) {
    // update version of computation
    // its is okay for buf_size_k to be null in this case
    zip_buffer(buf_size_2k, sketch.levels_[ending_level]);
  } else {
    // merge_into version of computation
    std::move(&buf_size_k[0], &buf_size_k[0] + k, &sketch.levels_[ending_level][0]);
  }

  for (uint64_t lvl = starting_level; lvl < ending_level; lvl++) {
    assert((bit_pattern & (static_cast<uint64_t>(1) << lvl)) > 0); // internal consistency check
    merge_two_size_k_buffers(
        sketch.levels_[lvl],
        sketch.levels_[ending_level],
        buf_size_2k);
    sketch.levels_[lvl].clear();
    sketch.levels_[ending_level].clear();
    zip_buffer(buf_size_2k, sketch.levels_[ending_level]);
  } // end of loop over lower levels

  // update bit pattern with binary-arithmetic ripple carry
  sketch.bit_pattern_ = bit_pattern + (static_cast<uint64_t>(1) << starting_level);
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::zip_buffer(Level& buf_in, Level& buf_out) {
#ifdef QUANTILES_VALIDATION
  static uint32_t next_offset = 0;
  uint32_t rand_offset = next_offset;
  next_offset = 1 - next_offset;
#else
  uint32_t rand_offset = random_bit();
#endif
  assert(buf_in.size() == 2 * buf_out.capacity());
  assert(buf_out.size() == 0);
  size_t k = buf_out.capacity();
  for (uint32_t i = rand_offset, o = 0; o < k; i += 2, ++o) {
    buf_out.push_back(std::move(buf_in[i]));
  }
  buf_in.clear();
}

template<typename T, typename C, typename A>
void quantiles_sketch<T, C, A>::merge_two_size_k_buffers(Level& src_1, Level& src_2, Level& dst) {
  assert(src_1.size() == src_2.size());
  assert(src_1.size() * 2 == dst.capacity());
  assert(dst.size() == 0);

  auto end1 = src_1.end(), end2 = src_2.end();
  auto it1 = src_1.begin(), it2 = src_2.begin();
  
  // TODO: probably actually doing copies given Level&?
  while (it1 != end1 && it2 != end2) {
    if (C()(*it1, *it2)) {
      dst.push_back(std::move(*it1++));
    } else {
      dst.push_back(std::move(*it2++));
    }
  }

  if (it1 != end1) {
    dst.insert(dst.end(), it1, end1);
  } else {
    assert(it2 != end2);
    dst.insert(dst.end(), it2, end2);
  }
}

template<typename T, typename C, typename A>
int quantiles_sketch<T, C, A>::lowest_zero_bit_starting_at(uint64_t bits, uint8_t starting_bit) {
  uint8_t pos = starting_bit & 0X3F;
  uint64_t my_bits = bits >> pos;

  while ((my_bits & static_cast<uint64_t>(1)) != 0) {
    my_bits >>= 1;
    pos++;
  }
  return pos;
}

template<typename T, typename C, typename A>
class quantiles_sketch<T, C, A>::item_deleter {
  public:
  item_deleter(const A& allocator): allocator_(allocator) {}
  void operator() (T* ptr) {
    if (ptr != nullptr) {
      ptr->~T();
      allocator_.deallocate(ptr, 1);
    }
  }
  private:
  A allocator_;
};

template<typename T, typename C, typename A>
class quantiles_sketch<T, C, A>::items_deleter {
  public:
  items_deleter(const A& allocator, bool destroy, size_t num): allocator_(allocator), destroy_(destroy), num_(num) {}
  void operator() (T* ptr) {
    if (ptr != nullptr) {
      if (destroy_) {
        for (size_t i = 0; i < num_; ++i) {
          ptr[i].~T();
        }
      }
      allocator_.deallocate(ptr, num_);
    }
  }
  void set_destroy(bool destroy) { destroy_ = destroy; }
  private:
  A allocator_;
  bool destroy_;
  size_t num_;
};


// quantiles_sketch::const_iterator implementation

template<typename T, typename C, typename A>
quantiles_sketch<T, C, A>::const_iterator::const_iterator(const Level& base_buffer,
                                                             const std::vector<Level, AllocLevel>& levels,
                                                             uint16_t k,
                                                             uint64_t n,
                                                             bool is_end):
base_buffer_(base_buffer),
levels_(levels),
level_(-1),
index_(0),
k_(k),
bb_count_(compute_base_buffer_items(k, n)),
bit_pattern_(compute_bit_pattern(k, n)),
weight_(1)
{
  if (is_end) {
    // if exact mode: index_ = n is end
    // if sampling, level_ = max_level + 1 and index_ = 0 is end
    if (bit_pattern_ == 0) // only a valid check for exact mode in constructor
      index_ = static_cast<uint32_t>(n);
    else
      level_ = levels_.size();
  } else { // find first non-empty item
    if (bb_count_ == 0 && bit_pattern_ > 0) {
      level_ = 0;
      weight_ = 2;
      while ((bit_pattern_ & 0x01) == 0) {
        weight_ *= 2;
        ++level_;
        bit_pattern_ >>= 1;
      }
    }
  }
}

template<typename T, typename C, typename A>
typename quantiles_sketch<T, C, A>::const_iterator& quantiles_sketch<T, C, A>::const_iterator::operator++() {
  ++index_;

  if ((level_ == -1 && index_ == base_buffer_.size() && levels_.size() > 0) || (level_ >= 0 && index_ == k_)) { // go to the next non-empty level
    index_ = 0;
    do {
      ++level_;
      if (level_ > 0) bit_pattern_ = bit_pattern_ >> 1;
      if (bit_pattern_ == 0) return *this;
      weight_ *= 2;
    } while ((bit_pattern_ & static_cast<uint64_t>(1)) == 0);
  }
  return *this;
}

template<typename T, typename C, typename A>
typename quantiles_sketch<T, C, A>::const_iterator& quantiles_sketch<T, C, A>::const_iterator::operator++(int) {
  const_iterator tmp(*this);
  operator++();
  return tmp;
}

template<typename T, typename C, typename A>
bool quantiles_sketch<T, C, A>::const_iterator::operator==(const const_iterator& other) const {
  return level_ == other.level_ && index_ == other.index_;
}

template<typename T, typename C, typename A>
bool quantiles_sketch<T, C, A>::const_iterator::operator!=(const const_iterator& other) const {
  return !operator==(other);
}

template<typename T, typename C, typename A>
std::pair<const T&, const uint64_t> quantiles_sketch<T, C, A>::const_iterator::operator*() const {
  return std::pair<const T&, const uint64_t>(level_ == -1 ? base_buffer_[index_] : levels_[level_][index_], weight_);
}

} /* namespace datasketches */

#endif // _QUANTILES_SKETCH_IMPL_HPP_