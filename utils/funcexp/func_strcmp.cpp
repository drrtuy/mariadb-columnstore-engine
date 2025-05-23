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

/****************************************************************************
 * $Id: func_strcmp.cpp 2477 2011-04-01 16:07:35Z rdempsey $
 *
 *
 ****************************************************************************/

#include <cstdlib>
#include <string>
#include <sstream>
using namespace std;

#include "functor_int.h"
#include "functioncolumn.h"
using namespace execplan;

#include "rowgroup.h"
using namespace rowgroup;

#include "joblisttypes.h"
using namespace joblist;

using namespace funcexp;

namespace funcexp
{
CalpontSystemCatalog::ColType Func_strcmp::operationType(FunctionParm& /*fp*/,
                                                         CalpontSystemCatalog::ColType& resultType)
{
  // operation type is not used by this functor
  // return fp[0]->data()->resultType();
  return resultType;
}

int64_t Func_strcmp::getIntVal(rowgroup::Row& row, FunctionParm& fp, bool& isNull,
                               execplan::CalpontSystemCatalog::ColType& /*type*/)
{
  CHARSET_INFO* cs = fp[0]->data()->resultType().getCharset();
  const auto& str = fp[0]->data()->getStrVal(row, isNull);
  const auto& str1 = fp[1]->data()->getStrVal(row, isNull);

  // XXX: str() results may be nullptrs.
  int ret = cs->strnncollsp(str.str(), str.length(), str1.str(), str1.length());
  // mysql's strcmp returns only -1, 0, and 1
  return (ret < 0 ? -1 : (ret > 0 ? 1 : 0));
}

std::string Func_strcmp::getStrVal(rowgroup::Row& row, FunctionParm& fp, bool& isNull,
                                   execplan::CalpontSystemCatalog::ColType& type)
{
  int64_t val = getIntVal(row, fp, isNull, type);

  if (val > 0)
    return string("1");
  else if (val < 0)
    return string("-1");
  else
    return string("0");
}

}  // namespace funcexp
