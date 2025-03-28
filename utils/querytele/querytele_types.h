/**
 * Autogenerated by Thrift Compiler (0.9.1)
 *
 * DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING
 *  @generated
 */
#pragma once

#include <thrift/Thrift.h>
#include <thrift/TApplicationException.h>
#include <thrift/protocol/TProtocol.h>
#include <thrift/transport/TTransport.h>

namespace querytele
{
struct QTType
{
  enum type
  {
    QT_INVALID = 0,
    QT_SUMMARY = 1,
    QT_PROGRESS = 2,
    QT_START = 3
  };
};

extern const std::map<int, const char*> _QTType_VALUES_TO_NAMES;

struct STType
{
  enum type
  {
    ST_INVALID = 0,
    ST_SUMMARY = 1,
    ST_PROGRESS = 2,
    ST_START = 3
  };
};

extern const std::map<int, const char*> _STType_VALUES_TO_NAMES;

struct StepType
{
  enum type
  {
    T_INVALID = 0,
    T_HJS = 1,
    T_DSS = 2,
    T_CES = 3,
    T_SQS = 4,
    T_TAS = 5,
    T_TNS = 6,
    T_BPS = 7,
    T_TCS = 8,
    T_HVS = 9,
    T_WFS = 10,
    T_SAS = 11,
    T_TUN = 12
  };
};

extern const std::map<int, const char*> _StepType_VALUES_TO_NAMES;

struct ITType
{
  enum type
  {
    IT_INVALID = 0,
    IT_SUMMARY = 1,
    IT_PROGRESS = 2,
    IT_START = 3,
    IT_TERM = 4
  };
};

extern const std::map<int, const char*> _ITType_VALUES_TO_NAMES;

typedef std::vector<std::string> StringList;

typedef std::vector<int64_t> I64List;

typedef struct _QueryTele__isset
{
  _QueryTele__isset()
   : query_uuid(false)
   , msg_type(false)
   , max_mem_pct(false)
   , num_files(false)
   , phy_io(false)
   , cache_io(false)
   , msg_rcv_cnt(false)
   , cp_blocks_skipped(false)
   , msg_bytes_in(false)
   , msg_bytes_out(false)
   , rows(false)
   , start_time(false)
   , end_time(false)
   , error_no(false)
   , blocks_changed(false)
   , session_id(false)
   , query_type(false)
   , query(false)
   , user(false)
   , host(false)
   , priority(false)
   , priority_level(false)
   , system_name(false)
   , module_name(false)
   , local_query(false)
   , schema_name(false)
  {
  }
  bool query_uuid;
  bool msg_type;
  bool max_mem_pct;
  bool num_files;
  bool phy_io;
  bool cache_io;
  bool msg_rcv_cnt;
  bool cp_blocks_skipped;
  bool msg_bytes_in;
  bool msg_bytes_out;
  bool rows;
  bool start_time;
  bool end_time;
  bool error_no;
  bool blocks_changed;
  bool session_id;
  bool query_type;
  bool query;
  bool user;
  bool host;
  bool priority;
  bool priority_level;
  bool system_name;
  bool module_name;
  bool local_query;
  bool schema_name;
} _QueryTele__isset;

class QueryTele
{
 public:
  static const char* ascii_fingerprint;  // = "E179941E2BE9C920811D86261E5AAE67";
  static const uint8_t binary_fingerprint
      [16];  // = {0xE1,0x79,0x94,0x1E,0x2B,0xE9,0xC9,0x20,0x81,0x1D,0x86,0x26,0x1E,0x5A,0xAE,0x67};

  QueryTele()
   : query_uuid()
   , msg_type((QTType::type)0)
   , max_mem_pct(0)
   , num_files(0)
   , phy_io(0)
   , cache_io(0)
   , msg_rcv_cnt(0)
   , cp_blocks_skipped(0)
   , msg_bytes_in(0)
   , msg_bytes_out(0)
   , rows(0)
   , start_time(0)
   , end_time(0)
   , error_no(0)
   , blocks_changed(0)
   , session_id(0)
   , query_type()
   , query()
   , user()
   , host()
   , priority()
   , priority_level(0)
   , system_name()
   , module_name()
   , local_query(0)
   , schema_name()
  {
  }

  virtual ~QueryTele() throw() = default;

  std::string query_uuid;
  QTType::type msg_type;
  int64_t max_mem_pct;
  int64_t num_files;
  int64_t phy_io;
  int64_t cache_io;
  int64_t msg_rcv_cnt;
  int64_t cp_blocks_skipped;
  int64_t msg_bytes_in;
  int64_t msg_bytes_out;
  int64_t rows;
  int64_t start_time;
  int64_t end_time;
  int64_t error_no;
  int64_t blocks_changed;
  int64_t session_id;
  std::string query_type;
  std::string query;
  std::string user;
  std::string host;
  std::string priority;
  int32_t priority_level;
  std::string system_name;
  std::string module_name;
  int32_t local_query;
  std::string schema_name;

  _QueryTele__isset __isset;

  void __set_query_uuid(const std::string& val)
  {
    query_uuid = val;
  }

  void __set_msg_type(const QTType::type val)
  {
    msg_type = val;
  }

  void __set_max_mem_pct(const int64_t val)
  {
    max_mem_pct = val;
    __isset.max_mem_pct = true;
  }

  void __set_num_files(const int64_t val)
  {
    num_files = val;
    __isset.num_files = true;
  }

  void __set_phy_io(const int64_t val)
  {
    phy_io = val;
    __isset.phy_io = true;
  }

  void __set_cache_io(const int64_t val)
  {
    cache_io = val;
    __isset.cache_io = true;
  }

  void __set_msg_rcv_cnt(const int64_t val)
  {
    msg_rcv_cnt = val;
    __isset.msg_rcv_cnt = true;
  }

  void __set_cp_blocks_skipped(const int64_t val)
  {
    cp_blocks_skipped = val;
    __isset.cp_blocks_skipped = true;
  }

  void __set_msg_bytes_in(const int64_t val)
  {
    msg_bytes_in = val;
    __isset.msg_bytes_in = true;
  }

  void __set_msg_bytes_out(const int64_t val)
  {
    msg_bytes_out = val;
    __isset.msg_bytes_out = true;
  }

  void __set_rows(const int64_t val)
  {
    rows = val;
    __isset.rows = true;
  }

  void __set_start_time(const int64_t val)
  {
    start_time = val;
    __isset.start_time = true;
  }

  void __set_end_time(const int64_t val)
  {
    end_time = val;
    __isset.end_time = true;
  }

  void __set_error_no(const int64_t val)
  {
    error_no = val;
    __isset.error_no = true;
  }

  void __set_blocks_changed(const int64_t val)
  {
    blocks_changed = val;
    __isset.blocks_changed = true;
  }

  void __set_session_id(const int64_t val)
  {
    session_id = val;
    __isset.session_id = true;
  }

  void __set_query_type(const std::string& val)
  {
    query_type = val;
    __isset.query_type = true;
  }

  void __set_query(const std::string& val)
  {
    query = val;
    __isset.query = true;
  }

  void __set_user(const std::string& val)
  {
    user = val;
    __isset.user = true;
  }

  void __set_host(const std::string& val)
  {
    host = val;
    __isset.host = true;
  }

  void __set_priority(const std::string& val)
  {
    priority = val;
    __isset.priority = true;
  }

  void __set_priority_level(const int32_t val)
  {
    priority_level = val;
    __isset.priority_level = true;
  }

  void __set_system_name(const std::string& val)
  {
    system_name = val;
    __isset.system_name = true;
  }

  void __set_module_name(const std::string& val)
  {
    module_name = val;
    __isset.module_name = true;
  }

  void __set_local_query(const int32_t val)
  {
    local_query = val;
    __isset.local_query = true;
  }

  void __set_schema_name(const std::string& val)
  {
    schema_name = val;
    __isset.schema_name = true;
  }

  bool operator==(const QueryTele& rhs) const
  {
    if (!(query_uuid == rhs.query_uuid))
      return false;

    if (!(msg_type == rhs.msg_type))
      return false;

    if (__isset.max_mem_pct != rhs.__isset.max_mem_pct)
      return false;
    else if (__isset.max_mem_pct && !(max_mem_pct == rhs.max_mem_pct))
      return false;

    if (__isset.num_files != rhs.__isset.num_files)
      return false;
    else if (__isset.num_files && !(num_files == rhs.num_files))
      return false;

    if (__isset.phy_io != rhs.__isset.phy_io)
      return false;
    else if (__isset.phy_io && !(phy_io == rhs.phy_io))
      return false;

    if (__isset.cache_io != rhs.__isset.cache_io)
      return false;
    else if (__isset.cache_io && !(cache_io == rhs.cache_io))
      return false;

    if (__isset.msg_rcv_cnt != rhs.__isset.msg_rcv_cnt)
      return false;
    else if (__isset.msg_rcv_cnt && !(msg_rcv_cnt == rhs.msg_rcv_cnt))
      return false;

    if (__isset.cp_blocks_skipped != rhs.__isset.cp_blocks_skipped)
      return false;
    else if (__isset.cp_blocks_skipped && !(cp_blocks_skipped == rhs.cp_blocks_skipped))
      return false;

    if (__isset.msg_bytes_in != rhs.__isset.msg_bytes_in)
      return false;
    else if (__isset.msg_bytes_in && !(msg_bytes_in == rhs.msg_bytes_in))
      return false;

    if (__isset.msg_bytes_out != rhs.__isset.msg_bytes_out)
      return false;
    else if (__isset.msg_bytes_out && !(msg_bytes_out == rhs.msg_bytes_out))
      return false;

    if (__isset.rows != rhs.__isset.rows)
      return false;
    else if (__isset.rows && !(rows == rhs.rows))
      return false;

    if (__isset.start_time != rhs.__isset.start_time)
      return false;
    else if (__isset.start_time && !(start_time == rhs.start_time))
      return false;

    if (__isset.end_time != rhs.__isset.end_time)
      return false;
    else if (__isset.end_time && !(end_time == rhs.end_time))
      return false;

    if (__isset.error_no != rhs.__isset.error_no)
      return false;
    else if (__isset.error_no && !(error_no == rhs.error_no))
      return false;

    if (__isset.blocks_changed != rhs.__isset.blocks_changed)
      return false;
    else if (__isset.blocks_changed && !(blocks_changed == rhs.blocks_changed))
      return false;

    if (__isset.session_id != rhs.__isset.session_id)
      return false;
    else if (__isset.session_id && !(session_id == rhs.session_id))
      return false;

    if (__isset.query_type != rhs.__isset.query_type)
      return false;
    else if (__isset.query_type && !(query_type == rhs.query_type))
      return false;

    if (__isset.query != rhs.__isset.query)
      return false;
    else if (__isset.query && !(query == rhs.query))
      return false;

    if (__isset.user != rhs.__isset.user)
      return false;
    else if (__isset.user && !(user == rhs.user))
      return false;

    if (__isset.host != rhs.__isset.host)
      return false;
    else if (__isset.host && !(host == rhs.host))
      return false;

    if (__isset.priority != rhs.__isset.priority)
      return false;
    else if (__isset.priority && !(priority == rhs.priority))
      return false;

    if (__isset.priority_level != rhs.__isset.priority_level)
      return false;
    else if (__isset.priority_level && !(priority_level == rhs.priority_level))
      return false;

    if (__isset.system_name != rhs.__isset.system_name)
      return false;
    else if (__isset.system_name && !(system_name == rhs.system_name))
      return false;

    if (__isset.module_name != rhs.__isset.module_name)
      return false;
    else if (__isset.module_name && !(module_name == rhs.module_name))
      return false;

    if (__isset.local_query != rhs.__isset.local_query)
      return false;
    else if (__isset.local_query && !(local_query == rhs.local_query))
      return false;

    if (__isset.schema_name != rhs.__isset.schema_name)
      return false;
    else if (__isset.schema_name && !(schema_name == rhs.schema_name))
      return false;

    return true;
  }
  bool operator!=(const QueryTele& rhs) const
  {
    return !(*this == rhs);
  }

  bool operator<(const QueryTele&) const;

  uint32_t read(::apache::thrift::protocol::TProtocol* iprot);
  uint32_t write(::apache::thrift::protocol::TProtocol* oprot) const;
};

void swap(QueryTele& a, QueryTele& b);

typedef struct _StepTele__isset
{
  _StepTele__isset()
   : query_uuid(false)
   , msg_type(false)
   , step_type(false)
   , step_uuid(false)
   , phy_io(false)
   , cache_io(false)
   , msg_rcv_cnt(false)
   , cp_blocks_skipped(false)
   , msg_bytes_in(false)
   , msg_bytes_out(false)
   , rows(false)
   , start_time(false)
   , end_time(false)
   , total_units_of_work(false)
   , units_of_work_completed(false)
  {
  }
  bool query_uuid;
  bool msg_type;
  bool step_type;
  bool step_uuid;
  bool phy_io;
  bool cache_io;
  bool msg_rcv_cnt;
  bool cp_blocks_skipped;
  bool msg_bytes_in;
  bool msg_bytes_out;
  bool rows;
  bool start_time;
  bool end_time;
  bool total_units_of_work;
  bool units_of_work_completed;
} _StepTele__isset;

class StepTele
{
 public:
  static const char* ascii_fingerprint;  // = "4E40C17AE92DFF833A8CC23318F4FE68";
  static const uint8_t binary_fingerprint
      [16];  // = {0x4E,0x40,0xC1,0x7A,0xE9,0x2D,0xFF,0x83,0x3A,0x8C,0xC2,0x33,0x18,0xF4,0xFE,0x68};

  StepTele()
   : query_uuid()
   , msg_type((STType::type)0)
   , step_type((StepType::type)0)
   , step_uuid()
   , phy_io(0)
   , cache_io(0)
   , msg_rcv_cnt(0)
   , cp_blocks_skipped(0)
   , msg_bytes_in(0)
   , msg_bytes_out(0)
   , rows(0)
   , start_time(0)
   , end_time(0)
   , total_units_of_work(0)
   , units_of_work_completed(0)
  {
  }

  virtual ~StepTele() throw() = default;

  std::string query_uuid;
  STType::type msg_type;
  StepType::type step_type;
  std::string step_uuid;
  int64_t phy_io;
  int64_t cache_io;
  int64_t msg_rcv_cnt;
  int64_t cp_blocks_skipped;
  int64_t msg_bytes_in;
  int64_t msg_bytes_out;
  int64_t rows;
  int64_t start_time;
  int64_t end_time;
  int32_t total_units_of_work;
  int32_t units_of_work_completed;

  _StepTele__isset __isset;

  void __set_query_uuid(const std::string& val)
  {
    query_uuid = val;
  }

  void __set_msg_type(const STType::type val)
  {
    msg_type = val;
  }

  void __set_step_type(const StepType::type val)
  {
    step_type = val;
  }

  void __set_step_uuid(const std::string& val)
  {
    step_uuid = val;
  }

  void __set_phy_io(const int64_t val)
  {
    phy_io = val;
    __isset.phy_io = true;
  }

  void __set_cache_io(const int64_t val)
  {
    cache_io = val;
    __isset.cache_io = true;
  }

  void __set_msg_rcv_cnt(const int64_t val)
  {
    msg_rcv_cnt = val;
    __isset.msg_rcv_cnt = true;
  }

  void __set_cp_blocks_skipped(const int64_t val)
  {
    cp_blocks_skipped = val;
    __isset.cp_blocks_skipped = true;
  }

  void __set_msg_bytes_in(const int64_t val)
  {
    msg_bytes_in = val;
    __isset.msg_bytes_in = true;
  }

  void __set_msg_bytes_out(const int64_t val)
  {
    msg_bytes_out = val;
    __isset.msg_bytes_out = true;
  }

  void __set_rows(const int64_t val)
  {
    rows = val;
    __isset.rows = true;
  }

  void __set_start_time(const int64_t val)
  {
    start_time = val;
    __isset.start_time = true;
  }

  void __set_end_time(const int64_t val)
  {
    end_time = val;
    __isset.end_time = true;
  }

  void __set_total_units_of_work(const int32_t val)
  {
    total_units_of_work = val;
    __isset.total_units_of_work = true;
  }

  void __set_units_of_work_completed(const int32_t val)
  {
    units_of_work_completed = val;
    __isset.units_of_work_completed = true;
  }

  bool operator==(const StepTele& rhs) const
  {
    if (!(query_uuid == rhs.query_uuid))
      return false;

    if (!(msg_type == rhs.msg_type))
      return false;

    if (!(step_type == rhs.step_type))
      return false;

    if (!(step_uuid == rhs.step_uuid))
      return false;

    if (__isset.phy_io != rhs.__isset.phy_io)
      return false;
    else if (__isset.phy_io && !(phy_io == rhs.phy_io))
      return false;

    if (__isset.cache_io != rhs.__isset.cache_io)
      return false;
    else if (__isset.cache_io && !(cache_io == rhs.cache_io))
      return false;

    if (__isset.msg_rcv_cnt != rhs.__isset.msg_rcv_cnt)
      return false;
    else if (__isset.msg_rcv_cnt && !(msg_rcv_cnt == rhs.msg_rcv_cnt))
      return false;

    if (__isset.cp_blocks_skipped != rhs.__isset.cp_blocks_skipped)
      return false;
    else if (__isset.cp_blocks_skipped && !(cp_blocks_skipped == rhs.cp_blocks_skipped))
      return false;

    if (__isset.msg_bytes_in != rhs.__isset.msg_bytes_in)
      return false;
    else if (__isset.msg_bytes_in && !(msg_bytes_in == rhs.msg_bytes_in))
      return false;

    if (__isset.msg_bytes_out != rhs.__isset.msg_bytes_out)
      return false;
    else if (__isset.msg_bytes_out && !(msg_bytes_out == rhs.msg_bytes_out))
      return false;

    if (__isset.rows != rhs.__isset.rows)
      return false;
    else if (__isset.rows && !(rows == rhs.rows))
      return false;

    if (__isset.start_time != rhs.__isset.start_time)
      return false;
    else if (__isset.start_time && !(start_time == rhs.start_time))
      return false;

    if (__isset.end_time != rhs.__isset.end_time)
      return false;
    else if (__isset.end_time && !(end_time == rhs.end_time))
      return false;

    if (__isset.total_units_of_work != rhs.__isset.total_units_of_work)
      return false;
    else if (__isset.total_units_of_work && !(total_units_of_work == rhs.total_units_of_work))
      return false;

    if (__isset.units_of_work_completed != rhs.__isset.units_of_work_completed)
      return false;
    else if (__isset.units_of_work_completed && !(units_of_work_completed == rhs.units_of_work_completed))
      return false;

    return true;
  }
  bool operator!=(const StepTele& rhs) const
  {
    return !(*this == rhs);
  }

  bool operator<(const StepTele&) const;

  uint32_t read(::apache::thrift::protocol::TProtocol* iprot);
  uint32_t write(::apache::thrift::protocol::TProtocol* oprot) const;
};

void swap(StepTele& a, StepTele& b);

typedef struct _ImportTele__isset
{
  _ImportTele__isset()
   : job_uuid(false)
   , import_uuid(false)
   , msg_type(false)
   , start_time(false)
   , end_time(false)
   , table_list(false)
   , rows_so_far(false)
   , system_name(false)
   , module_name(false)
   , schema_name(false)
  {
  }
  bool job_uuid;
  bool import_uuid;
  bool msg_type;
  bool start_time;
  bool end_time;
  bool table_list;
  bool rows_so_far;
  bool system_name;
  bool module_name;
  bool schema_name;
} _ImportTele__isset;

class ImportTele
{
 public:
  static const char* ascii_fingerprint;  // = "9C47642F47F2AFBEA98238934EB1A211";
  static const uint8_t binary_fingerprint
      [16];  // = {0x9C,0x47,0x64,0x2F,0x47,0xF2,0xAF,0xBE,0xA9,0x82,0x38,0x93,0x4E,0xB1,0xA2,0x11};

  ImportTele()
   : job_uuid()
   , import_uuid()
   , msg_type((ITType::type)0)
   , start_time(0)
   , end_time(0)
   , system_name()
   , module_name()
   , schema_name()
  {
  }

  virtual ~ImportTele() throw() = default;

  std::string job_uuid;
  std::string import_uuid;
  ITType::type msg_type;
  int64_t start_time;
  int64_t end_time;
  StringList table_list;
  I64List rows_so_far;
  std::string system_name;
  std::string module_name;
  std::string schema_name;

  _ImportTele__isset __isset;

  void __set_job_uuid(const std::string& val)
  {
    job_uuid = val;
  }

  void __set_import_uuid(const std::string& val)
  {
    import_uuid = val;
  }

  void __set_msg_type(const ITType::type val)
  {
    msg_type = val;
  }

  void __set_start_time(const int64_t val)
  {
    start_time = val;
    __isset.start_time = true;
  }

  void __set_end_time(const int64_t val)
  {
    end_time = val;
    __isset.end_time = true;
  }

  void __set_table_list(const StringList& val)
  {
    table_list = val;
    __isset.table_list = true;
  }

  void __set_rows_so_far(const I64List& val)
  {
    rows_so_far = val;
    __isset.rows_so_far = true;
  }

  void __set_system_name(const std::string& val)
  {
    system_name = val;
    __isset.system_name = true;
  }

  void __set_module_name(const std::string& val)
  {
    module_name = val;
    __isset.module_name = true;
  }

  void __set_schema_name(const std::string& val)
  {
    schema_name = val;
    __isset.schema_name = true;
  }

  bool operator==(const ImportTele& rhs) const
  {
    if (!(job_uuid == rhs.job_uuid))
      return false;

    if (!(import_uuid == rhs.import_uuid))
      return false;

    if (!(msg_type == rhs.msg_type))
      return false;

    if (__isset.start_time != rhs.__isset.start_time)
      return false;
    else if (__isset.start_time && !(start_time == rhs.start_time))
      return false;

    if (__isset.end_time != rhs.__isset.end_time)
      return false;
    else if (__isset.end_time && !(end_time == rhs.end_time))
      return false;

    if (__isset.table_list != rhs.__isset.table_list)
      return false;
    else if (__isset.table_list && !(table_list == rhs.table_list))
      return false;

    if (__isset.rows_so_far != rhs.__isset.rows_so_far)
      return false;
    else if (__isset.rows_so_far && !(rows_so_far == rhs.rows_so_far))
      return false;

    if (__isset.system_name != rhs.__isset.system_name)
      return false;
    else if (__isset.system_name && !(system_name == rhs.system_name))
      return false;

    if (__isset.module_name != rhs.__isset.module_name)
      return false;
    else if (__isset.module_name && !(module_name == rhs.module_name))
      return false;

    if (__isset.schema_name != rhs.__isset.schema_name)
      return false;
    else if (__isset.schema_name && !(schema_name == rhs.schema_name))
      return false;

    return true;
  }
  bool operator!=(const ImportTele& rhs) const
  {
    return !(*this == rhs);
  }

  bool operator<(const ImportTele&) const;

  uint32_t read(::apache::thrift::protocol::TProtocol* iprot);
  uint32_t write(::apache::thrift::protocol::TProtocol* oprot) const;
};

void swap(ImportTele& a, ImportTele& b);

}  // namespace querytele
