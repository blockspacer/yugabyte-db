// Copyright (c) YugaByte, Inc.
//
// This file contains the YQLValue class that represents YQL values.

#include "yb/common/yql_value.h"

#include <cfloat>

#include <glog/logging.h>

#include "yb/common/wire_protocol.h"
#include "yb/gutil/strings/escaping.h"
#include "yb/util/bytes_formatter.h"
#include "yb/util/date_time.h"
#include "yb/util/decimal.h"

// The list of unsupported datypes to use in switch statements
#define YQL_UNSUPPORTED_TYPES_IN_SWITCH \
  case NULL_VALUE_TYPE: FALLTHROUGH_INTENDED; \
  case VARINT: FALLTHROUGH_INTENDED;    \
  case TUPLE: FALLTHROUGH_INTENDED;     \
  case TYPEARGS: FALLTHROUGH_INTENDED;  \
  case UNKNOWN_DATA

#define YQL_INVALID_TYPES_IN_SWITCH     \
  case UINT8:  FALLTHROUGH_INTENDED;    \
  case UINT16: FALLTHROUGH_INTENDED;    \
  case UINT32: FALLTHROUGH_INTENDED;    \
  case UINT64

namespace yb {

using std::string;
using std::shared_ptr;
using std::to_string;
using util::Decimal;
using util::FormatBytesAsStr;

template<typename T>
static int GenericCompare(const T& lhs, const T& rhs) {
  if (lhs < rhs) return -1;
  if (lhs > rhs) return 1;
  return 0;
}

YQLValue::~YQLValue() {}

//------------------------- instance methods for abstract YQLValue class -----------------------

int YQLValue::CompareTo(const YQLValue& other) const {
  CHECK_EQ(type(), other.type());
  CHECK(!IsNull());
  CHECK(!other.IsNull());
  switch (type()) {
    case InternalType::kInt8Value:   return GenericCompare(int8_value(), other.int8_value());
    case InternalType::kInt16Value:  return GenericCompare(int16_value(), other.int16_value());
    case InternalType::kInt32Value:  return GenericCompare(int32_value(), other.int32_value());
    case InternalType::kInt64Value:  return GenericCompare(int64_value(), other.int64_value());
    case InternalType::kFloatValue:  return GenericCompare(float_value(), other.float_value());
    case InternalType::kDoubleValue: return GenericCompare(double_value(), other.double_value());
    // Encoded decimal is byte-comparable.
    case InternalType::kDecimalValue: return decimal_value().compare(other.decimal_value());
    case InternalType::kStringValue: return string_value().compare(other.string_value());
    case InternalType::kBoolValue:
      LOG(FATAL) << "Internal error: bool type not comparable";
      return 0;
    case InternalType::kTimestampValue:
      return GenericCompare(timestamp_value(), other.timestamp_value());
    case InternalType::kBinaryValue: return binary_value().compare(other.binary_value());
    case InternalType::kInetaddressValue:
      return GenericCompare(inetaddress_value(), other.inetaddress_value());
    case InternalType::kUuidValue:
      return GenericCompare(uuid_value(), other.uuid_value());
    case InternalType::kTimeuuidValue:
      return GenericCompare(timeuuid_value(), other.timeuuid_value());
    case YQLValuePB::kMapValue: FALLTHROUGH_INTENDED;
    case YQLValuePB::kSetValue: FALLTHROUGH_INTENDED;
    case YQLValuePB::kListValue:
      LOG(FATAL) << "Internal error: collection types are not comparable";
      return 0;
    case InternalType::kVarintValue:
      LOG(FATAL) << "Internal error: varint not implemented";
    case InternalType::VALUE_NOT_SET:
      LOG(FATAL) << "Internal error: value should not be null";
      break;

    // default: fall through
  }

  LOG(FATAL) << "Internal error: unsupported type " << type();
  return 0;
}

void YQLValue::Serialize(
    const std::shared_ptr<YQLType>& yql_type, const YQLClient& client, faststring* buffer) const {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  if (IsNull()) {
    CQLEncodeLength(-1, buffer);
    return;
  }

  switch (yql_type->main()) {
    case INT8:
      CQLEncodeNum(Store8, int8_value(), buffer);
      return;
    case INT16:
      CQLEncodeNum(NetworkByteOrder::Store16, int16_value(), buffer);
      return;
    case INT32:
      CQLEncodeNum(NetworkByteOrder::Store32, int32_value(), buffer);
      return;
    case INT64:
      CQLEncodeNum(NetworkByteOrder::Store64, int64_value(), buffer);
      return;
    case FLOAT:
      CQLEncodeFloat(NetworkByteOrder::Store32, float_value(), buffer);
      return;
    case DOUBLE:
      CQLEncodeFloat(NetworkByteOrder::Store64, double_value(), buffer);
      return;
    case DECIMAL: {
      auto decimal = util::DecimalFromComparable(decimal_value());
      bool is_out_of_range = false;
      CQLEncodeBytes(decimal.EncodeToSerializedBigDecimal(&is_out_of_range), buffer);
      if(is_out_of_range) {
        LOG(ERROR) << "Out of range: Unable to encode decimal " << decimal.ToString()
                   << " into a BigDecimal serialized representation";
      }
      return;
    }
    case STRING:
      CQLEncodeBytes(string_value(), buffer);
      return;
    case BOOL:
      CQLEncodeNum(Store8, static_cast<uint8>(bool_value() ? 1 : 0), buffer);
      return;
    case BINARY:
      CQLEncodeBytes(binary_value(), buffer);
      return;
    case TIMESTAMP: {
      int64_t val = DateTime::AdjustPrecision(timestamp_value().ToInt64(),
          DateTime::internal_precision,
          DateTime::CqlDateTimeInputFormat.input_precision());
      CQLEncodeNum(NetworkByteOrder::Store64, val, buffer);
      return;
    }
    case INET: {
      std::string bytes;
      CHECK_OK(inetaddress_value().ToBytes(&bytes));
      CQLEncodeBytes(bytes, buffer);
      return;
    }
    case UUID: {
      std::string bytes;
      CHECK_OK(uuid_value().ToBytes(&bytes));
      CQLEncodeBytes(bytes, buffer);
      return;
    }
    case TIMEUUID: {
      std::string bytes;
      Uuid uuid = timeuuid_value();
      CHECK_OK(uuid.IsTimeUuid());
      CHECK_OK(uuid.ToBytes(&bytes));
      CQLEncodeBytes(bytes, buffer);
      return;
    }
    case MAP: {
      YQLMapValuePB map = map_value();
      DCHECK_EQ(map.keys_size(), map.values_size());
      int32_t start_pos = CQLStartCollection(buffer);
      int32_t length = static_cast<int32_t>(map.keys_size());
      CQLEncodeLength(length, buffer);
      const shared_ptr<YQLType>& keys_type = yql_type->params()[0];
      const shared_ptr<YQLType>& values_type = yql_type->params()[1];
      for (int i = 0; i < length; i++) {
        YQLValueWithPB(map.keys(i)).Serialize(keys_type, client, buffer);
        YQLValueWithPB(map.values(i)).Serialize(values_type, client, buffer);
      }
      CQLFinishCollection(start_pos, buffer);
      return;
    }
    case SET: {
      YQLSeqValuePB set = set_value();
      int32_t start_pos = CQLStartCollection(buffer);
      int32_t length = static_cast<int32_t>(set.elems_size());
      CQLEncodeLength(length, buffer); // number of elements in collection
      const shared_ptr<YQLType>& elems_type = yql_type->params()[0];
      for (auto& elem : set.elems()) {
        YQLValueWithPB(elem).Serialize(elems_type, client, buffer);
      }
      CQLFinishCollection(start_pos, buffer);
      return;
    }
    case LIST: {
      YQLSeqValuePB list = list_value();
      int32_t start_pos = CQLStartCollection(buffer);
      int32_t length = static_cast<int32_t>(list.elems_size());
      CQLEncodeLength(length, buffer);
      const shared_ptr<YQLType>& elems_type = yql_type->params()[0];
      for (auto& elem : list.elems()) {
        YQLValueWithPB(elem).Serialize(elems_type, client, buffer);
      }
      CQLFinishCollection(start_pos, buffer);
      return;
    }

    YQL_UNSUPPORTED_TYPES_IN_SWITCH:
      break;

    YQL_INVALID_TYPES_IN_SWITCH:
      break;
    // default: fall through
  }

  LOG(FATAL) << "Internal error: unsupported type " << yql_type->ToString();
}

Status YQLValue::Deserialize(
    const std::shared_ptr<YQLType>& yql_type, const YQLClient& client, Slice* data) {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  int32_t len = 0;
  RETURN_NOT_OK(CQLDecodeNum(sizeof(len), NetworkByteOrder::Load32, data, &len));
  if (len == -1) {
    SetNull();
    return Status::OK();
  }

  switch (yql_type->main()) {
    case INT8:
      return CQLDeserializeNum(
          len, Load8, static_cast<void (YQLValue::*)(int8_t)>(&YQLValue::set_int8_value), data);
    case INT16:
      return CQLDeserializeNum(
          len, NetworkByteOrder::Load16,
          static_cast<void (YQLValue::*)(int16_t)>(&YQLValue::set_int16_value), data);
    case INT32:
      return CQLDeserializeNum(
          len, NetworkByteOrder::Load32,
          static_cast<void (YQLValue::*)(int32_t)>(&YQLValue::set_int32_value), data);
    case INT64:
      return CQLDeserializeNum(
          len, NetworkByteOrder::Load64,
          static_cast<void (YQLValue::*)(int64_t)>(&YQLValue::set_int64_value), data);
    case FLOAT:
      return CQLDeserializeFloat(
          len, NetworkByteOrder::Load32,
          static_cast<void (YQLValue::*)(float)>(&YQLValue::set_float_value), data);
    case DOUBLE:
      return CQLDeserializeFloat(
          len, NetworkByteOrder::Load64,
          static_cast<void (YQLValue::*)(double)>(&YQLValue::set_double_value), data);
    case DECIMAL: {
      string value;
      RETURN_NOT_OK(CQLDecodeBytes(len, data, &value));
      Decimal decimal;
      RETURN_NOT_OK(decimal.DecodeFromSerializedBigDecimal(value));
      set_decimal_value(decimal.EncodeToComparable());
      return Status::OK();
    }
    case STRING:
      return CQLDecodeBytes(len, data, mutable_string_value());
    case BOOL: {
      uint8_t value = 0;
      RETURN_NOT_OK(CQLDecodeNum(len, Load8, data, &value));
      set_bool_value(value != 0);
      return Status::OK();
    }
    case BINARY:
      return CQLDecodeBytes(len, data, mutable_binary_value());
    case TIMESTAMP: {
      int64_t value = 0;
      RETURN_NOT_OK(CQLDecodeNum(len, NetworkByteOrder::Load64, data, &value));
      value = DateTime::AdjustPrecision(value,
          DateTime::CqlDateTimeInputFormat.input_precision(), DateTime::internal_precision);
      set_timestamp_value(value);
      return Status::OK();
    }
    case INET: {
      string bytes;
      RETURN_NOT_OK(CQLDecodeBytes(len, data, &bytes));
      InetAddress addr;
      RETURN_NOT_OK(addr.FromBytes(bytes));
      set_inetaddress_value(addr);
      return Status::OK();
    }
    case UUID: {
      string bytes;
      RETURN_NOT_OK(CQLDecodeBytes(len, data, &bytes));
      Uuid uuid;
      RETURN_NOT_OK(uuid.FromBytes(bytes));
      set_uuid_value(uuid);
      return Status::OK();
    }
    case TIMEUUID: {
      string bytes;
      RETURN_NOT_OK(CQLDecodeBytes(len, data, &bytes));
      Uuid uuid;
      RETURN_NOT_OK(uuid.FromBytes(bytes));
      RETURN_NOT_OK(uuid.IsTimeUuid());
      set_timeuuid_value(uuid);
      return Status::OK();
    }
    case MAP: {
      const shared_ptr<YQLType>& keys_type = yql_type->params()[0];
      const shared_ptr<YQLType>& values_type = yql_type->params()[1];
      set_map_value();
      int32_t nr_elems = 0;
      RETURN_NOT_OK(CQLDecodeNum(sizeof(nr_elems), NetworkByteOrder::Load32, data, &nr_elems));
      for (int i = 0; i < nr_elems; i++) {
        YQLValueWithPB key;
        RETURN_NOT_OK(key.Deserialize(keys_type, client, data));
        // TODO (mihnea) refactor YQLValue to avoid copying here and below
        add_map_key()->CopyFrom(key.value());
        YQLValueWithPB value;
        RETURN_NOT_OK(value.Deserialize(values_type, client, data));
        add_map_value()->CopyFrom(value.value());
      }
      return Status::OK();
    }
    case SET: {
      const shared_ptr<YQLType>& elems_type = yql_type->params()[0];
      set_set_value();
      int32_t nr_elems = 0;
      RETURN_NOT_OK(CQLDecodeNum(sizeof(nr_elems), NetworkByteOrder::Load32, data, &nr_elems));
      for (int i = 0; i < nr_elems; i++) {
        YQLValueWithPB elem;
        RETURN_NOT_OK(elem.Deserialize(elems_type, client, data));
        add_set_elem()->CopyFrom(elem.value());
      }
      return Status::OK();
    }
    case LIST: {
      const shared_ptr<YQLType>& elems_type = yql_type->params()[0];
      set_list_value();
      int32_t nr_elems = 0;
      RETURN_NOT_OK(CQLDecodeNum(sizeof(nr_elems), NetworkByteOrder::Load32, data, &nr_elems));
      for (int i = 0; i < nr_elems; i++) {
        YQLValueWithPB elem;;
        RETURN_NOT_OK(elem.Deserialize(elems_type, client, data));
        add_list_elem()->CopyFrom(elem.value());
      }
      return Status::OK();
    }

    YQL_UNSUPPORTED_TYPES_IN_SWITCH:
      break;

    YQL_INVALID_TYPES_IN_SWITCH:
      break;

    // default: fall through
  }

  LOG(FATAL) << "Internal error: unsupported type " << yql_type->ToString();
  return STATUS(InternalError, "unsupported type");
}

string YQLValue::ToString() const {
  if (IsNull()) {
    return "null";
  }

  switch (type()) {
    case InternalType::kInt8Value: return "int8:" + to_string(int8_value());
    case InternalType::kInt16Value: return "int16:" + to_string(int16_value());
    case InternalType::kInt32Value: return "int32:" + to_string(int32_value());
    case InternalType::kInt64Value: return "int64:" + to_string(int64_value());
    case InternalType::kFloatValue: return "float:" + to_string(float_value());
    case InternalType::kDoubleValue: return "double:" + to_string(double_value());
    case InternalType::kDecimalValue:
      return "decimal: " + util::DecimalFromComparable(decimal_value()).ToString();
    case InternalType::kStringValue: return "string:" + FormatBytesAsStr(string_value());
    case InternalType::kTimestampValue: return "timestamp:" + timestamp_value().ToFormattedString();
    case InternalType::kInetaddressValue: return "inetaddress:" + inetaddress_value().ToString();
    case InternalType::kUuidValue: return "uuid:" + uuid_value().ToString();
    case InternalType::kTimeuuidValue: return "timeuuid:" + timeuuid_value().ToString();
    case InternalType::kBoolValue: return (bool_value() ? "bool:true" : "bool:false");
    case InternalType::kBinaryValue: return "binary:0x" + b2a_hex(binary_value());
    case InternalType::kMapValue: {
      std::stringstream ss;
      YQLMapValuePB map = map_value();
      DCHECK_EQ(map.keys_size(), map.values_size());
      ss << "map:{";
      for (int i = 0; i < map.keys_size(); i++) {
        if (i > 0) {
          ss << ", ";
        }
        ss << YQLValueWithPB(map.keys(i)).ToString() << " -> "
           << YQLValueWithPB(map.values(i)).ToString();
      }
      ss << "}";
      return ss.str();
    }
    case InternalType::kSetValue: {
      std::stringstream ss;
      YQLSeqValuePB set = set_value();
      ss << "set:{";
      for (int i = 0; i < set.elems_size(); i++) {
        if (i > 0) {
          ss << ", ";
        }
        ss << YQLValueWithPB(set.elems(i)).ToString();
      }
      ss << "}";
      return ss.str();
    }
    case InternalType::kListValue: {
      std::stringstream ss;
      YQLSeqValuePB list = list_value();
      ss << "list:[";
      for (int i = 0; i < list.elems_size(); i++) {
        if (i > 0) {
          ss << ", ";
        }
        ss << YQLValueWithPB(list.elems(i)).ToString();
      }
      ss << "]";
      return ss.str();
    }

    case InternalType::kVarintValue:
      LOG(FATAL) << "Internal error: varint not implemented";
    case InternalType::VALUE_NOT_SET:
      LOG(FATAL) << "Internal error: value should not be null";
      return "null";
    // default: fall through
  }

  LOG(FATAL) << "Internal error: unknown or unsupported type " << type();
  return "unknown";
}

//------------------------- static functions for existing YQLValuePB --------------------------
void YQLValue::SetNull(YQLValuePB* v) {
  switch(v->value_case()) {
    case YQLValuePB::kInt8Value:   v->clear_int8_value(); return;
    case YQLValuePB::kInt16Value:  v->clear_int16_value(); return;
    case YQLValuePB::kInt32Value:  v->clear_int32_value(); return;
    case YQLValuePB::kInt64Value:  v->clear_int64_value(); return;
    case YQLValuePB::kFloatValue:  v->clear_float_value(); return;
    case YQLValuePB::kDoubleValue: v->clear_double_value(); return;
    case YQLValuePB::kDecimalValue: v->clear_decimal_value(); return;
    case YQLValuePB::kStringValue: v->clear_string_value(); return;
    case YQLValuePB::kBoolValue:   v->clear_bool_value(); return;
    case YQLValuePB::kTimestampValue: v->clear_timestamp_value(); return;
    case YQLValuePB::kBinaryValue: v->clear_binary_value(); return;
    case YQLValuePB::kInetaddressValue: v->clear_inetaddress_value(); return;
    case YQLValuePB::kUuidValue:   v->clear_uuid_value(); return;
    case YQLValuePB::kTimeuuidValue:    v->clear_timeuuid_value(); return;
    case YQLValuePB::kMapValue:    v->clear_map_value(); return;
    case YQLValuePB::kSetValue:    v->clear_set_value(); return;
    case YQLValuePB::kListValue:   v->clear_list_value(); return;
    case YQLValuePB::kVarintValue:
      LOG(FATAL) << "Internal error: varint not implemented";
    case YQLValuePB::VALUE_NOT_SET: return;
  }
  LOG(FATAL) << "Internal error: unknown or unsupported type " << v->value_case();
}

int YQLValue::CompareTo(const YQLValuePB& lhs, const YQLValuePB& rhs) {
  CHECK(Comparable(lhs, rhs));
  CHECK(BothNotNull(lhs, rhs));
  switch (lhs.value_case()) {
    case YQLValuePB::kInt8Value:   return GenericCompare(lhs.int8_value(), rhs.int8_value());
    case YQLValuePB::kInt16Value:  return GenericCompare(lhs.int16_value(), rhs.int16_value());
    case YQLValuePB::kInt32Value:  return GenericCompare(lhs.int32_value(), rhs.int32_value());
    case YQLValuePB::kInt64Value:  return GenericCompare(lhs.int64_value(), rhs.int64_value());
    case YQLValuePB::kFloatValue:  return GenericCompare(lhs.float_value(), rhs.float_value());
    case YQLValuePB::kDoubleValue: return GenericCompare(lhs.double_value(), rhs.double_value());
    // Encoded decimal is byte-comparable.
    case YQLValuePB::kDecimalValue: return lhs.decimal_value().compare(rhs.decimal_value());
    case YQLValuePB::kStringValue: return lhs.string_value().compare(rhs.string_value());
    case YQLValuePB::kBoolValue:
      LOG(FATAL) << "Internal error: bool type not comparable";
      return 0;
    case YQLValuePB::kTimestampValue:
      return GenericCompare(lhs.timestamp_value(), rhs.timestamp_value());
    case YQLValuePB::kBinaryValue: return lhs.binary_value().compare(rhs.binary_value());
    case YQLValuePB::kInetaddressValue:
      return GenericCompare(lhs.inetaddress_value(), rhs.inetaddress_value());
    case YQLValuePB::kUuidValue:
      return GenericCompare(lhs.uuid_value(), rhs.uuid_value());
    case YQLValuePB::kTimeuuidValue:
      return GenericCompare(lhs.timeuuid_value(), rhs.timeuuid_value());
    case YQLValuePB::kMapValue: FALLTHROUGH_INTENDED;
    case YQLValuePB::kSetValue: FALLTHROUGH_INTENDED;
    case YQLValuePB::kListValue:
      LOG(FATAL) << "Internal error: collection types are not comparable";
      return 0;
    case InternalType::kVarintValue:
      LOG(FATAL) << "Internal error: varint not implemented";
    case YQLValuePB::VALUE_NOT_SET:
      LOG(FATAL) << "Internal error: value should not be null";
      break;

    // default: fall through
  }

  LOG(FATAL) << "Internal error: unknown or unsupported type " << lhs.value_case();
  return 0;
}

//----------------------------------- YQLValuePB operators --------------------------------

#define YQL_COMPARE(lhs, rhs, op)                                       \
  do { return YQLValue::BothNotNull(lhs, rhs) && YQLValue::CompareTo(lhs, rhs) op 0; } while (0)

bool operator <(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, <); }
bool operator >(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, >); }
bool operator <=(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, <=); }
bool operator >=(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, >=); }
bool operator ==(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, ==); }
bool operator !=(const YQLValuePB& lhs, const YQLValuePB& rhs) { YQL_COMPARE(lhs, rhs, !=); }

#undef YQL_COMPARE

} // namespace yb
