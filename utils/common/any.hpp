#pragma once
/*
 * (C) Copyright Christopher Diggins 2005-2011
 * (C) Copyright Pablo Aguilar 2005
 * (C) Copyright Kevlin Henney 2001
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt
 */

#include <cstdint>
#include <stdexcept>
#include <cstring>
#include "mcs_basic_types.h"

namespace static_any
{
namespace anyimpl
{
struct empty_any
{
};

struct base_any_policy
{
  virtual void static_delete(void** x) = 0;
  virtual void copy_from_value(void const* src, void** dest) = 0;
  virtual void clone(void* const* src, void** dest) = 0;
  virtual void move(void* const* src, void** dest) = 0;
  virtual void* get_value(void** src) = 0;
  virtual size_t get_size() = 0;

 protected:
  ~base_any_policy() = default;
};

// inline base_any_policy::~base_any_policy() throw () {}

template <typename T>
struct typed_base_any_policy : base_any_policy
{
  size_t get_size() override
  {
    return sizeof(T);
  }

 protected:
  ~typed_base_any_policy() = default;
};

template <typename T>
struct small_any_policy : typed_base_any_policy<T>
{
  virtual ~small_any_policy() = default;
  void static_delete(void** x) override
  {
    *x = nullptr;
  }
  void copy_from_value(void const* src, void** dest) override
  {
    new (dest) T(*reinterpret_cast<T const*>(src));
  }
  void clone(void* const* src, void** dest) override
  {
    *dest = *src;
  }
  void move(void* const* src, void** dest) override
  {
    *dest = *src;
  }
  void* get_value(void** src) override
  {
    return reinterpret_cast<void*>(src);
  }
};

template <typename T>
struct big_any_policy : typed_base_any_policy<T>
{
  virtual ~big_any_policy() = default;
  void static_delete(void** x) override
  {
    if (*x)
      delete (*reinterpret_cast<T**>(x));
    *x = nullptr;
  }
  void copy_from_value(void const* src, void** dest) override
  {
    *dest = new T(*reinterpret_cast<T const*>(src));
  }
  void clone(void* const* src, void** dest) override
  {
    *dest = new T(**reinterpret_cast<T* const*>(src));
  }
  void move(void* const* src, void** dest) override
  {
    (*reinterpret_cast<T**>(dest))->~T();
    **reinterpret_cast<T**>(dest) = **reinterpret_cast<T* const*>(src);
  }
  void* get_value(void** src) override
  {
    return *src;
  }
};

template <typename T>
struct choose_policy
{
  typedef big_any_policy<T> type;
};

template <typename T>
struct choose_policy<T*>
{
  typedef small_any_policy<T*> type;
};

struct any;

/// Choosing the policy for an any type is illegal, but should never happen.
/// This is designed to throw a compiler error.
template <>
struct choose_policy<any>
{
  typedef void type;
};

/// Specializations for big types.
#define BIG_POLICY(TYPE)               \
  template <>                          \
  struct choose_policy<TYPE>           \
  {                                    \
    typedef big_any_policy<TYPE> type; \
  };

BIG_POLICY(int128_t);
BIG_POLICY(long double);

/// Specializations for small types.
#define SMALL_POLICY(TYPE)               \
  template <>                            \
  struct choose_policy<TYPE>             \
  {                                      \
    typedef small_any_policy<TYPE> type; \
  };

SMALL_POLICY(char);
SMALL_POLICY(signed char);
SMALL_POLICY(unsigned char);
SMALL_POLICY(signed short);
SMALL_POLICY(unsigned short);
SMALL_POLICY(signed int);
SMALL_POLICY(unsigned int);
SMALL_POLICY(signed long);
SMALL_POLICY(unsigned long);
SMALL_POLICY(signed long long);
SMALL_POLICY(unsigned long long);
SMALL_POLICY(float);
SMALL_POLICY(double);
SMALL_POLICY(bool);

#undef SMALL_POLICY

/// This function will return a different policy for each type.
template <typename T>
base_any_policy* get_policy()
{
  static typename choose_policy<T>::type policy;
  return &policy;
};
}  // namespace anyimpl

class any
{
 private:
  // fields
  anyimpl::base_any_policy* policy;
  void* object;

 public:
  /// Initializing constructor.
  template <typename T>
  any(const T& x) : policy(anyimpl::get_policy<anyimpl::empty_any>()), object(nullptr)
  {
    assign(x);
  }

  /// Empty constructor.
  any() : policy(anyimpl::get_policy<anyimpl::empty_any>()), object(nullptr)
  {
  }

  /// Special initializing constructor for string literals.
  any(const char* x) : policy(anyimpl::get_policy<anyimpl::empty_any>()), object(nullptr)
  {
    assign(x);
  }

  /// Copy constructor.
  any(const any& x) : policy(anyimpl::get_policy<anyimpl::empty_any>()), object(nullptr)
  {
    assign(x);
  }

  /// Destructor.
  ~any()
  {
    policy->static_delete(&object);
  }

  /// Assignment function from another any.
  any& assign(const any& x)
  {
    reset();
    policy = x.policy;
    policy->clone(&x.object, &object);
    return *this;
  }

  /// Assignment function.
  template <typename T>
  any& assign(const T& x)
  {
    reset();
    policy = anyimpl::get_policy<T>();
    policy->copy_from_value(&x, &object);
    return *this;
  }

  /// Assignment operator.
  template <typename T>
  any& operator=(const T& x)
  {
    return assign(x);
  }

  /// Assignment operator, specialed for literal strings.
  /// They have types like const char [6] which don't work as expected.
  any& operator=(const char* x)
  {
    return assign(x);
  }

  /// Less than operator for sorting
  bool operator<(const any& x) const
  {
    if (policy == x.policy)
    {
      void* p1 = const_cast<void*>(object);
      void* p2 = const_cast<void*>(x.object);
      return memcmp(policy->get_value(&p1), x.policy->get_value(&p2), policy->get_size()) < 0 ? 1 : 0;
    }
    return 0;
  }

  /// equal operator
  bool operator==(const any& x) const
  {
    if (policy == x.policy)
    {
      void* p1 = const_cast<void*>(object);
      void* p2 = const_cast<void*>(x.object);
      return memcmp(policy->get_value(&p1), x.policy->get_value(&p2), policy->get_size()) == 0 ? 1 : 0;
    }
    return 0;
  }

  /// Utility functions
  uint8_t getHash() const
  {
    void* p1 = const_cast<void*>(object);
    return *(uint64_t*)policy->get_value(&p1) % 4048;
  }
  any& swap(any& x)
  {
    std::swap(policy, x.policy);
    std::swap(object, x.object);
    return *this;
  }

  /// Cast operator. You can only cast to the original type.
  template <typename T>
  T& cast()
  {
    if (policy != anyimpl::get_policy<T>())
      throw std::runtime_error("static_any: type mismatch in cast");
    T* r = reinterpret_cast<T*>(policy->get_value(&object));
    return *r;
  }

  /// Returns true if the any contains no value.
  bool empty() const
  {
    return policy == anyimpl::get_policy<anyimpl::empty_any>();
  }

  /// Frees any allocated memory, and sets the value to NULL.
  void reset()
  {
    policy->static_delete(&object);
    policy = anyimpl::get_policy<anyimpl::empty_any>();
  }

  /// Returns true if the two types are the same.
  bool compatible(const any& x) const
  {
    return policy == x.policy;
  }
};

}  // namespace static_any
