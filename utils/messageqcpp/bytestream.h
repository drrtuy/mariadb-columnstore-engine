/*
   Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2019 MariaDB Corporation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.
*/

#pragma once
#include <optional>
#include <string>
#include <iostream>
#include <sys/types.h>
#include <stdexcept>
#include <vector>
#include <set>
#include <boost/shared_ptr.hpp>

#include <boost/version.hpp>
#include <boost/uuid/uuid.hpp>
#include <stdint.h>
#include <cstring>

#include "mcs_basic_types.h"
#include "exceptclasses.h"
#include "serializeable.h"
#include "any.hpp"
#include "nullstring.h"
#include "countingallocator.h"
#include "buffertypes.h"

class ByteStreamTestSuite;

#define EXPORT

namespace messageqcpp
{
typedef boost::shared_ptr<ByteStream> SBS;
using BSSizeType = uint64_t;
using BSBufType = uint8_t;
/**
 * @brief A class to marshall bytes as a stream
 *
 * The ByteStream class is used to marshall numeric data into and out of a stream of
 * bytes (unsigned chars). It is a FIFO queue that maintains current input and output
 * pointers.
 *
 * @warning Alas, due to recent changes this class no longer implements a strong execption guarantee.
 *
 * @warning the current implementation does not know how to compact memory, so it should be
 * destructed or have reset() called to clear out the current uint8_t array. Also, multi-uint8_t
 * numeric values are pushed and dequeued in the native byte order, so they are not portable
 * across machines with different byte orders.
 *
 */
class ByteStream : public Serializeable
{
 public:
  // We now use the standard Linux types of uint8_t, uint16_t, etc.
  // These are kept around for backward compatibility
  typedef uint8_t byte;
  typedef uint16_t doublebyte;
  typedef uint32_t quadbyte;
  typedef uint64_t octbyte;
  typedef int128_t hexbyte;
  typedef boost::uuids::uuid uuid;

  /**
   *	default ctor
   */
  EXPORT explicit ByteStream(BSSizeType initSize = 8192);  // multiples of pagesize are best
  explicit ByteStream(allocators::CountingAllocator<BSBufType>& alloc, uint32_t initSize = 8192);
  /**
   *	ctor with a uint8_t array and len initializer
   */
  inline ByteStream(const uint8_t* bp, BSSizeType len);
  /**
   *	copy ctor
   */
  EXPORT ByteStream(const ByteStream& rhs);
  EXPORT ByteStream(const SBS& rhs);
  /**
   *	assign op
   */
  EXPORT ByteStream& operator=(const ByteStream& rhs);
  /**
   *	assign op
   */
  inline ByteStream& operator=(const SBS& rhs);
  /**
   *	dtor
   */
  inline ~ByteStream() override;

  /**
   *	push a int8_t onto the end of the stream
   */
  EXPORT ByteStream& operator<<(int8_t b);
  /**
   *	push a uint8_t onto the end of the stream
   */
  EXPORT ByteStream& operator<<(uint8_t b);
  /**
   *	push a int16_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(int16_t d);
  /**
   *	push a uint16_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(uint16_t d);
  /**
   *	push a int32_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(int32_t q);
  /**
   *	push a uint32_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(uint32_t q);
  /**
   *	push an int64_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(int64_t o);
  /**
   *	push an uint64_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(uint64_t o);
  /**
   *  push an int128_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(const int128_t& o);

  /**
   *	push an uint128_t onto the end of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(const uint128_t& o);
  /**
   *  push a float onto the end of the stream. The byte order is
   *  whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(float f);
  /**
   *  push a double onto the end of the stream. The byte order is
   *  whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(double d);
  /**
   *  push a long double onto the end of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator<<(long double d);
  /**
   * push a std::string onto the end of the stream.
   */
  EXPORT ByteStream& operator<<(const std::string& s);
  /**
   * push a NullString onto the end of the stream.
   */
  EXPORT ByteStream& operator<<(const utils::NullString& s);
  /**
   * push an arbitrary class onto the end of the stream.
   */
  inline ByteStream& operator<<(const Serializeable& s);
  /**
   * push a ByteStream onto the end of the stream.
   */
  EXPORT ByteStream& operator<<(const ByteStream& bs);
  /**
   * push a UUID onto the end of the stream.
   */
  EXPORT ByteStream& operator<<(const uuid& u);

  /**
   *	extract a int8_t from the front of the stream.
   */
  EXPORT ByteStream& operator>>(int8_t& b);
  /**
   *	extract a uint8_t from the front of the stream.
   */
  EXPORT ByteStream& operator>>(uint8_t& b);
  /**
   *	extract a int16_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(int16_t& d);
  /**
   *	extract a uint16_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(uint16_t& d);
  /**
   *	extract a int32_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(int32_t& q);
  /**
   *	extract a uint32_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(uint32_t& q);
  /**
   *	extract an int64_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(int64_t& o);
  /**
   *	extract an uint64_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(uint64_t& o);
  /**
   *  extract an int128_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(int128_t& o);
  /**
   *	extract an uint128_t from the front of the stream. The byte order is whatever the native byte order
   *is.
   */
  EXPORT ByteStream& operator>>(uint128_t& o);
  /**
   *  extract a float from the front of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(float& f);
  /**
   *  extract a double from the front of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(double& d);
  /**
   *  extract a long double from the front of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT ByteStream& operator>>(long double& d);
  /**
   * extract a std::string from the front of the stream.
   */
  EXPORT ByteStream& operator>>(std::string& s);
  /**
   * extract a NullString from the front of the stream.
   */
  EXPORT ByteStream& operator>>(utils::NullString& s);
  /**
   *	write the current stream into b. The ByteStream will be empty after this operation.
   * @warning the caller is responsible for making sure b is big enough to hold all the data (perhaps by
   * calling length()).
   */
  EXPORT ByteStream& operator>>(uint8_t*& b);
  /**
   * extract an arbitrary object from the front of the stream.
   */
  inline ByteStream& operator>>(Serializeable& s);
  /**
   * extract a ByteStream from the front of the stream.
   */
  EXPORT ByteStream& operator>>(ByteStream& bs);
  /**
   * extract a UUID from the front of the stream.
   */
  EXPORT ByteStream& operator>>(uuid& u);

  /**
   *	Peek at a int8_t from the front of the stream.
   */
  EXPORT void peek(int8_t& b) const;
  /**
   *	Peek at a uint8_t from the front of the stream.
   */
  EXPORT void peek(uint8_t& b) const;
  /**
   *	Peek at a int16_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(int16_t& d) const;
  /**
   *	Peek at a uint16_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(uint16_t& d) const;
  /**
   *	Peek at a int32_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(int32_t& q) const;
  /**
   *	Peek at a uint32_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(uint32_t& q) const;
  /**
   *	Peek at an int64_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(int64_t& o) const;
  /**
   *	Peek at an uint64_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(uint64_t& o) const;
  /**
   *  Peek at an int128_t from the front of the stream. The byte order is whatever the native byte order is.
   */
  EXPORT void peek(int128_t& o) const;
  /**
   *	Peek at an uint128_t from the front of the stream. The byte order is whatever the native byte order
   *is.
   */
  EXPORT void peek(uint128_t& o) const;
  /**
   *  Peek at a float from the front of the stream. The byte order
   *  is whatever the native byte order is.
   */
  EXPORT void peek(float& f) const;
  /**
   *  Peek at a double from the front of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT void peek(double& f) const;
  /**
   *  Peek at a long double from the front of the stream. The byte
   *  order is whatever the native byte order is.
   */
  EXPORT void peek(long double& f) const;
  /**
   * Peek at a std::string from the front of the stream.
   */
  EXPORT void peek(std::string& s) const;
  /**
   * Peek at the whole ByteStream buffer.
   * @warning the caller is responsible for making sure b is big enough to hold all the data (perhaps by
   * calling length()).
   */
  inline void peek(uint8_t*& b) const;
  /**
   * Peek at a ByteStream from the front of the stream.
   */
  EXPORT void peek(ByteStream& bs) const;
  /**
   * Peek at a UUID from the front of the stream.
   */
  EXPORT void peek(uuid& u) const;

  /**
   *	load the stream from an array. Clears out any previous data.
   */
  EXPORT void load(const uint8_t* bp, BSSizeType len);

  /**
   *	append bytes to the end of the stream.
   */
  EXPORT void append(const uint8_t* bp, BSSizeType len);

  /**
   * equality check on buffer contents.
   */
  EXPORT bool operator==(const ByteStream& b) const;

  /**
   * inequality check on buffer contents.
   */
  EXPORT bool operator!=(const ByteStream& b) const;

  /**
   * these fcns are an alternative to code like {build a msg, bs.load or bs.append}.
   * This will let us build the msg directly in the BS buffer.
   */
  EXPORT void needAtLeast(size_t amount);
  inline uint8_t* getInputPtr();
  inline void advanceInputPtr(size_t amount);

  /**
   *	returns a const pointer to the current head of the queue.  If you use it for
   *  raw access, you might want to advance the current head.
   */
  inline const uint8_t* buf() const;

  /**
   *	returns a pointer to the current head of the queue.  If you use it for
   *  raw access, you might want to advance the current head.
   */
  inline uint8_t* buf();

  /**
   *	advance the output ptr without having to extract bytes
   * @warning be careful advancing near 4GB!
   */
  inline void advance(BSSizeType amt);

  /**
   *	returns the length of the queue (in bytes)
   * @warning do not attempt to make a ByteStream bigger than 4GB!
   */
  inline BSSizeType length() const;
  inline bool empty() const;

  /**
   *	returns the length of the queue, including header overhead (in bytes)
   */
  inline BSSizeType lengthWithHdrOverhead() const;

  /**
   *	clears the stream. Releases any current stream and sets all pointers to 0. The state of the object
   * is identical to its state immediately after default construction.
   */
  inline void reset();

  /**
   * operator+=
   */
  inline ByteStream& operator+=(const ByteStream& rhs);

  /**
   * swap this ByteStream with another ByteStream
   */
  EXPORT void swap(ByteStream& rhs);

  /**
   * reset the input & output pointers to the beginning
   */
  inline void restart();

  /**
   * Move the input pointer back to the beginning so the contents
   * can be read again.
   */
  inline void rewind();

  /**
   * Get the allocated size of the buffer.
   */
  inline BSSizeType getBufferSize() const;

  /**
   * Serializeable interface
   */
  EXPORT void serialize(ByteStream& bs) const override;

  /**
   * Serializeable interface
   */
  EXPORT void deserialize(ByteStream& bs) override;

  /**
   *	memory allocation chunk size
   */
  EXPORT static const BSSizeType BlockSize = 4096;

  /** size of the space we want in front of the data */
  EXPORT static const BSSizeType ISSOverhead =
      3 * sizeof(uint32_t);  // space for the BS magic & length & number of long strings.

  friend class ::ByteStreamTestSuite;

 protected:
  /**
   *	pushes one uint8_t onto the end of the stream
   */
  void add(uint8_t b);
  /**
   *	adds another BlockSize bytes to the internal buffer
   */
  void growBuf(BSSizeType toSize = 0);
  /**
   *	handles member copying from one ByteStream to another
   */
  void doCopy(const ByteStream& rhs);

 private:
  BSBufType* allocate(const size_t size);
  void deallocate(BSBufType* ptr);

  BSBufType* fBuf;        /// the start of the allocated buffer
  BSBufType* fCurInPtr;   // the point in fBuf where data is inserted next
  BSBufType* fCurOutPtr;  // the point in fBuf where data is extracted from next
  BSSizeType fMaxLen;      // how big fBuf is currently
  std::optional<allocators::CountingAllocator<BSBufType>> allocator = {};
};

template <int W, typename T = void>
struct _ByteStreamType
{
  typedef T type;
};

template <int W>
struct ByteStreamType : _ByteStreamType<W>
{
};

template <>
struct ByteStreamType<1> : _ByteStreamType<1, ByteStream::byte>
{
};
template <>
struct ByteStreamType<2> : _ByteStreamType<2, ByteStream::doublebyte>
{
};
template <>
struct ByteStreamType<4> : _ByteStreamType<4, ByteStream::quadbyte>
{
};
template <>
struct ByteStreamType<8> : _ByteStreamType<8, ByteStream::octbyte>
{
};
template <>
struct ByteStreamType<16> : _ByteStreamType<16, ByteStream::hexbyte>
{
};

// type descriptors to let ByteStream point out protocol errors, WIP
static const uint8_t BS_UINT8 = 0;
static const uint8_t BS_UINT16 = 1;
static const uint8_t BS_UINT32 = 2;
static const uint8_t BS_UINT64 = 3;
static const uint8_t BS_INT8 = 4;
static const uint8_t BS_INT16 = 5;
static const uint8_t BS_INT32 = 6;
static const uint8_t BS_INT64 = 7;
static const uint8_t BS_STRING = 8;
static const uint8_t BS_BLOB = 9;
static const uint8_t BS_SERIALIZABLE = 10;
static const uint8_t BS_UUID = 11;

inline ByteStream::ByteStream(const uint8_t* bp, BSSizeType len) : fBuf(nullptr), fMaxLen(0)
{
  load(bp, len);
}
inline ByteStream::~ByteStream()
{
  deallocate(fBuf);
}

inline const uint8_t* ByteStream::buf() const
{
  return fCurOutPtr;
}
inline uint8_t* ByteStream::buf()
{
  return fCurOutPtr;
}
inline BSSizeType ByteStream::length() const
{
  return static_cast<BSSizeType>(fCurInPtr - fCurOutPtr);
}
inline bool ByteStream::empty() const
{
  return (length() == 0);
}
inline BSSizeType ByteStream::lengthWithHdrOverhead() const
{
  return (length() + ISSOverhead);
}
inline void ByteStream::reset()
{
  deallocate(fBuf);
  fMaxLen = 0;
  fCurInPtr = fCurOutPtr = fBuf = nullptr;
}
inline void ByteStream::restart()
{
  fCurInPtr = fCurOutPtr = fBuf + ISSOverhead;
}
inline void ByteStream::rewind()
{
  fCurOutPtr = fBuf + ISSOverhead;
}
inline void ByteStream::advance(BSSizeType adv)
{
  // fCurOutPtr is always >= fBuf, so fCurOutPtr - fBuf is >= 0, and this difference is always <= 32 bits
  // there is an edge condition not detected here: if fCurOutPtr - fBuf is nearly 4GB and you try to
  // advance by a lot, you will wrap over, so be warned!
  if (adv > length())
    throw std::length_error("ByteStream: advanced beyond the end of the buffer");

  fCurOutPtr += adv;
}
inline uint8_t* ByteStream::getInputPtr()
{
  return fCurInPtr;
}
inline void ByteStream::advanceInputPtr(size_t amount)
{
  fCurInPtr += amount;
}
inline void ByteStream::peek(uint8_t*& bpr) const
{
  memcpy(bpr, fCurOutPtr, length());
}

inline ByteStream& ByteStream::operator+=(const ByteStream& rhs)
{
  append(rhs.buf(), rhs.length());
  return *this;
}
inline ByteStream operator+(const ByteStream& lhs, const ByteStream& rhs)
{
  ByteStream temp(lhs);
  return temp += rhs;
}
inline ByteStream& ByteStream::operator>>(Serializeable& s)
{
  s.deserialize(*this);
  return *this;
}
inline ByteStream& ByteStream::operator<<(const Serializeable& s)
{
  s.serialize(*this);
  return *this;
}
inline ByteStream& ByteStream::operator=(const SBS& rhs)
{
  *this = *rhs;
  return *this;
}

inline BSSizeType ByteStream::getBufferSize() const
{
  return fMaxLen;
}

/**
 * stream a ByteStream out to any ostream
 */
inline std::ostream& operator<<(std::ostream& os, const ByteStream& bs)
{
  return os.write(reinterpret_cast<const char*>(bs.buf()), bs.length());
}

/**
 * stream a ByteStream in from a file
 */
EXPORT std::ifstream& operator>>(std::ifstream& os, ByteStream& bs);

/// Generic method to export a vector of T's that implement Serializeable
template <typename T>
void serializeVector(ByteStream& bs, const std::vector<T>& v)
{
  typename std::vector<T>::const_iterator it;
  uint64_t size;

  size = v.size();
  bs << size;

  for (it = v.begin(); it != v.end(); it++)
    bs << *it;
}

/// Generic method to deserialize a vector of T's that implement Serializeable
template <typename T>
void deserializeVector(ByteStream& bs, std::vector<T>& v)
{
  uint32_t i;
  T tmp;
  uint64_t size;

  v.clear();
  bs >> size;

  for (i = 0; i < size; i++)
  {
    bs >> tmp;
    v.push_back(tmp);
  }
}

template <typename T>
void serializeInlineVector(ByteStream& bs, const std::vector<T>& v)
{
  uint64_t size = v.size();
  bs << size;

  if (size > 0)
    bs.append((const uint8_t*)&(v[0]), sizeof(T) * size);
}

inline void serializeVector(ByteStream& bs, const std::vector<int64_t>& v)
{
  serializeInlineVector<int64_t>(bs, v);
}

template <typename T>
void deserializeInlineVector(ByteStream& bs, std::vector<T>& v)
{
  uint64_t size;
  const uint8_t* buf;

  v.clear();
  bs >> size;

  if (size > 0)
  {
    v.resize(size);
    buf = bs.buf();
    memcpy(&(v[0]), buf, sizeof(T) * size);
    bs.advance(sizeof(T) * size);
  }
}

inline void deserializeVector(ByteStream& bs, std::vector<int64_t>& v)
{
  deserializeInlineVector<int64_t>(bs, v);
}

/// Generic method to serialize a set of T's that implement Serializeable
template <typename T>
void serializeSet(ByteStream& bs, const std::set<T>& s)
{
  uint64_t size = s.size();
  bs << size;
  typename std::set<T>::const_iterator it;

  for (it = s.begin(); it != s.end(); ++it)
    bs << *it;
}

/// Generic method to deserialize a set of T's that implement Serializeable
template <typename T>
void deserializeSet(ByteStream& bs, std::set<T>& s)
{
  uint32_t i;
  T tmp;
  uint64_t size;

  s.clear();
  bs >> size;

  for (i = 0; i < size; i++)
  {
    bs >> tmp;
    s.insert(tmp);
  }
}

}  // namespace messageqcpp

namespace std
{
/** total specialization of std::swap
 *
 */
template <>
inline void swap<messageqcpp::ByteStream>(messageqcpp::ByteStream& lhs, messageqcpp::ByteStream& rhs)
{
  lhs.swap(rhs);
}
}  // namespace std

#undef EXPORT
