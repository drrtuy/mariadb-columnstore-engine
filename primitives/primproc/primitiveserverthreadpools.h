/* Copyright (C) 2022 MariaDB Corporation

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
   MA 02110-1301, USA. */

#pragma once

#include <boost/shared_ptr.hpp>
#include <utility>
#include "fair_threadpool.h"

class PrimitiveServerThreadPools
{
 public:
  PrimitiveServerThreadPools() = default;
  PrimitiveServerThreadPools(boost::shared_ptr<threadpool::FairThreadPool> primServerThreadPool)
   : fPrimServerThreadPool(std::move(primServerThreadPool))
  {
  }

  boost::shared_ptr<threadpool::FairThreadPool> getPrimitiveServerThreadPool()
  {
    return fPrimServerThreadPool;
  }

 private:
  boost::shared_ptr<threadpool::FairThreadPool> fPrimServerThreadPool;
};
