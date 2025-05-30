/* Copyright (C) 2014 InfiniDB, Inc.

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

#include "queryteleserverparms.h"

namespace querytele
{
class QueryTeleProtoImpl
{
 public:
  explicit QueryTeleProtoImpl(const QueryTeleServerParms&);
  ~QueryTeleProtoImpl() = default;

  int enqStepTele(const StepTele&);
  int enqImportTele(const ImportTele&);
  int enqQueryTele(const QueryTele&);

  /**
   * Wait for the consumer thread to post all messages
   **/
  int waitForQueues();

 protected:
 private:
  QueryTeleServerParms fServerParms;
};

}  // namespace querytele
