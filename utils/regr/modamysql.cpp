#include <my_config.h>
#include <cmath>
#include <string.h>
#include <unordered_map>
#include "idb_mysql.h"

namespace
{
inline bool isNumeric(int type, const char* attr)
{
  if (type == INT_RESULT || type == REAL_RESULT || type == DECIMAL_RESULT)
  {
    return true;
  }
  if (strncasecmp("NULL", attr, 4) == 0)
  {
    return true;
  }
  return false;
}

struct moda_data
{
  long double fSum;
  uint64_t fCount;
  enum Item_result fReturnType;
  std::unordered_map<int64_t, uint32_t> mapINT;
  std::unordered_map<double, uint32_t> mapREAL;
  std::unordered_map<long double, uint32_t> mapDECIMAL;
  std::string result;
  void clear()
  {
    fSum = 0.0;
    fCount = 0;
    mapINT.clear();
    mapREAL.clear();
    mapDECIMAL.clear();
  }
};
}  // namespace

template <class TYPE, class CONTAINER>
void moda(CONTAINER& container, struct moda_data* data)
{
  TYPE avg = (TYPE)data->fCount ? data->fSum / data->fCount : 0;
  TYPE val = 0.0;
  uint32_t maxCnt = 0.0;

  for (auto iter = container.begin(); iter != container.end(); ++iter)
  {
    if (iter->second > maxCnt)
    {
      val = iter->first;
      maxCnt = iter->second;
    }
    else if (iter->second == maxCnt)
    {
      // Tie breaker: choose the closest to avg. If still tie, choose smallest
      if ((abs(val - avg) > abs(iter->first - avg)) ||
          ((abs(val - avg) == abs(iter->first - avg)) && (abs(val) > abs(iter->first))))
      {
        val = iter->first;
      }
    }
  }

  data->result = std::to_string(val);
}

extern "C"
{
  my_bool moda_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    struct moda_data* data;

    if (args->arg_count != 1)
    {
      strcpy(message, "moda() requires exactly one argument");
      return 1;
    }
    if (!isNumeric(args->arg_type[0], args->attributes[0]))
    {
      if (args->arg_type[0] != STRING_RESULT)
      {
        strcpy(message, "moda() with an invalid argument");
        return 1;
      }
    }

    data = new moda_data;
    data->fReturnType = args->arg_type[0];
    data->fCount = 0;
    data->fSum = 0.0;
    initid->ptr = (char*)data;
    return 0;
  }

  void moda_deinit(UDF_INIT* initid)
  {
    struct moda_data* data = (struct moda_data*)initid->ptr;
    data->clear();
    delete data;
  }

  void moda_clear(UDF_INIT* initid, char* /*is_null*/, char* /*message*/)
  {
    struct moda_data* data = (struct moda_data*)initid->ptr;
    data->clear();
  }

  void moda_add(UDF_INIT* initid, UDF_ARGS* args, char* /*is_null*/, char* /*message*/)
  {
    // Test for NULL
    if (args->args[0] == 0)
    {
      return;
    }

    struct moda_data* data = (struct moda_data*)initid->ptr;
    data->fCount++;

    switch (args->arg_type[0])
    {
      case INT_RESULT:
      {
        int64_t val = *((int64_t*)args->args[0]);
        data->fSum += (long double)val;
        data->mapINT[val]++;
        break;
      }
      case REAL_RESULT:
      {
        double val = *((double*)args->args[0]);
        data->fSum += val;
        data->mapREAL[val]++;
        break;
      }
      case DECIMAL_RESULT:
      case STRING_RESULT:
      {
        long double val = strtold(args->args[0], 0);
        data->fSum += val;
        data->mapDECIMAL[val]++;
        break;
      }
      default: break;
    }
  }

  void moda_remove(UDF_INIT* initid, UDF_ARGS* args, char* /*is_null*/, char* /*message*/)
  {
    // Test for NULL
    if (args->args[0] == 0)
    {
      return;
    }
    struct moda_data* data = (struct moda_data*)initid->ptr;
    data->fCount--;

    switch (args->arg_type[0])
    {
      case INT_RESULT:
      {
        int64_t val = *((int64_t*)args->args[0]);
        data->fSum -= (long double)val;
        data->mapINT[val]--;
        break;
      }
      case REAL_RESULT:
      {
        double val = *((double*)args->args[0]);
        data->fSum -= val;
        data->mapREAL[val]--;
        break;
      }
      case DECIMAL_RESULT:
      case STRING_RESULT:
      {
        long double val = strtold(args->args[0], 0);
        data->fSum -= val;
        data->mapDECIMAL[val]--;
        break;
      }
      default: break;
    }
  }

  // char* moda(UDF_INIT* initid, UDF_ARGS* args, char* is_null, char* error __attribute__((unused)))
  char* moda(UDF_INIT* initid, UDF_ARGS* args, char* /*result*/, ulong* res_length, char* /*is_null*/,
             char* /*error*/)
  {
    struct moda_data* data = (struct moda_data*)initid->ptr;
    switch (args->arg_type[0])
    {
      case INT_RESULT: moda<int64_t>(data->mapINT, data); break;
      case REAL_RESULT: moda<double>(data->mapREAL, data); break;
      case DECIMAL_RESULT:
      case STRING_RESULT: moda<long double>(data->mapDECIMAL, data); break;
      default: return NULL;
    }
    *res_length = data->result.size();
    return const_cast<char*>(data->result.c_str());
  }
}  // Extern "C"
