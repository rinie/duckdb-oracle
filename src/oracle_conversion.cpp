#include "oracle_conversion.hpp"
#include "oracle_utils.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/decimal.hpp"

#include <cstring>
#include <cstdio>

namespace duckdb {

// Convert a dpiTimestamp to DuckDB timestamp (microseconds since epoch)
static timestamp_t OdpiTimestampToTimestamp(const dpiTimestamp &ts) {
	date_t d = Date::FromDate(ts.year, ts.month, ts.day);
	dtime_t t = Time::FromTime(ts.hour, ts.minute, ts.second,
	                            (int32_t)(ts.fsecond / 1000)); // nanosec -> microsec
	return Timestamp::FromDatetime(d, t);
}

// Convert a dpiTimestamp WITH TIME ZONE to DuckDB timestamp_tz_t
static timestamp_tz_t OdpiTimestampTZToTimestampTZ(const dpiTimestamp &ts) {
	// ODPI-C gives UTC offset in hours/minutes - convert to UTC first
	date_t d = Date::FromDate(ts.year, ts.month, ts.day);
	dtime_t t = Time::FromTime(ts.hour, ts.minute, ts.second,
	                            (int32_t)(ts.fsecond / 1000));
	timestamp_t utc_ts = Timestamp::FromDatetime(d, t);
	// Subtract the UTC offset
	int32_t offset_minutes = ts.tzHourOffset * 60 + ts.tzMinuteOffset;
	utc_ts = Timestamp::FromEpochMicroSeconds(
	    Timestamp::GetEpochMicroSeconds(utc_ts) - (int64_t)offset_minutes * 60 * 1e6);
	return timestamp_tz_t(Timestamp::GetEpochMicroSeconds(utc_ts));
}

void OracleConvertValue(Vector &col, idx_t out_row,
                         dpiData *data, dpiNativeTypeNum native_type,
                         const LogicalType &target_type,
                         const OracleType &oracle_type) {
	if (data->isNull) {
		FlatVector::SetNull(col, out_row, true);
		return;
	}

	auto target_id = target_type.id();

	switch (target_id) {
	case LogicalTypeId::BOOLEAN: {
		bool val = false;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = data->value.asInt64 != 0;
		} else if (native_type == DPI_NATIVE_TYPE_BOOLEAN) {
			val = data->value.asBoolean != 0;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			val = (s == "1" || s == "Y" || s == "y" || s == "TRUE" || s == "true");
		}
		FlatVector::GetDataMutable<bool>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::TINYINT: {
		int8_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int8_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int8_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int8_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<int8_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::SMALLINT: {
		int16_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int16_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int16_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int16_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<int16_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::INTEGER: {
		int32_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int32_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int32_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int32_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<int32_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::BIGINT: {
		int64_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int64_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int64_t)atoll(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<int64_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::FLOAT: {
		float val = 0.0f;
		if (native_type == DPI_NATIVE_TYPE_FLOAT) {
			val = data->value.asFloat;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (float)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (float)atof(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<float>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::DOUBLE: {
		double val = 0.0;
		if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_FLOAT) {
			val = (double)data->value.asFloat;
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (double)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = atof(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetDataMutable<double>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::DECIMAL: {
		// Oracle NUMBER comes back as DPI_NATIVE_TYPE_BYTES (string repr) by default
		double dval = 0.0;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			dval = atof(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			dval = data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			dval = (double)data->value.asInt64;
		}
		auto width = DecimalType::GetWidth(target_type);
		auto scale = DecimalType::GetScale(target_type);
		double multiplier = 1.0;
		for (int i = 0; i < scale; i++) {
			multiplier *= 10.0;
		}
		int64_t int_val = (int64_t)(dval * multiplier + (dval >= 0 ? 0.5 : -0.5));
		if (width <= Decimal::MAX_WIDTH_INT16) {
			FlatVector::GetDataMutable<int16_t>(col)[out_row] = (int16_t)int_val;
		} else if (width <= Decimal::MAX_WIDTH_INT32) {
			FlatVector::GetDataMutable<int32_t>(col)[out_row] = (int32_t)int_val;
		} else if (width <= Decimal::MAX_WIDTH_INT64) {
			FlatVector::GetDataMutable<int64_t>(col)[out_row] = int_val;
		} else {
			FlatVector::GetDataMutable<hugeint_t>(col)[out_row] = Hugeint::Convert(int_val);
		}
		break;
	}
	case LogicalTypeId::VARCHAR: {
		string str_val;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			str_val = string(data->value.asBytes.ptr, data->value.asBytes.length);
		} else if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			auto &ts = data->value.asTimestamp;
			char buf[64];
			snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
			         ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second,
			         ts.fsecond / 1000);
			str_val = buf;
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			str_val = to_string(data->value.asInt64);
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			str_val = to_string(data->value.asDouble);
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_DS) {
			auto &ivl = data->value.asIntervalDS;
			char buf[64];
			snprintf(buf, sizeof(buf), "%d %02d:%02d:%02d.%06d", ivl.days,
			         ivl.hours, ivl.minutes, ivl.seconds, ivl.fseconds / 1000);
			str_val = buf;
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_YM) {
			auto &ivl = data->value.asIntervalYM;
			char buf[32];
			snprintf(buf, sizeof(buf), "%d-%d", ivl.years, ivl.months);
			str_val = buf;
		}
		FlatVector::GetDataMutable<string_t>(col)[out_row] =
		    StringVector::AddString(col, str_val);
		break;
	}
	case LogicalTypeId::BLOB: {
		string blob_val;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			blob_val = string(data->value.asBytes.ptr, data->value.asBytes.length);
		}
		FlatVector::GetDataMutable<string_t>(col)[out_row] =
		    StringVector::AddStringOrBlob(col, blob_val);
		break;
	}
	case LogicalTypeId::DATE: {
		date_t d;
		if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			d = Date::FromDate(data->value.asTimestamp.year,
			                   data->value.asTimestamp.month,
			                   data->value.asTimestamp.day);
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			// ISO format string
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			bool special;
			d = Date::FromCString(s.c_str(), s.size(), special);
		} else {
			d = Date::FromDate(1970, 1, 1);
		}
		FlatVector::GetDataMutable<date_t>(col)[out_row] = d;
		break;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		timestamp_t ts;
		if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			if (target_id == LogicalTypeId::TIMESTAMP_TZ) {
				auto tz = OdpiTimestampTZToTimestampTZ(data->value.asTimestamp);
				FlatVector::GetDataMutable<timestamp_tz_t>(col)[out_row] = tz;
				return;
			}
			ts = OdpiTimestampToTimestamp(data->value.asTimestamp);
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			bool special;
			ts = Timestamp::FromCString(s.c_str(), s.size());
		} else {
			ts = Timestamp::FromEpochMicroSeconds(0);
		}
		FlatVector::GetDataMutable<timestamp_t>(col)[out_row] = ts;
		break;
	}
	case LogicalTypeId::INTERVAL: {
		interval_t ivl = {};
		if (native_type == DPI_NATIVE_TYPE_INTERVAL_DS) {
			auto &ds = data->value.asIntervalDS;
			ivl.days = ds.days;
			ivl.micros = ((int64_t)ds.hours * 3600 + (int64_t)ds.minutes * 60 +
			               ds.seconds) * 1000000LL + ds.fseconds / 1000;
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_YM) {
			// Approximate: months only
			auto &ym = data->value.asIntervalYM;
			ivl.months = ym.years * 12 + ym.months;
		}
		FlatVector::GetDataMutable<interval_t>(col)[out_row] = ivl;
		break;
	}
	default:
		FlatVector::SetNull(col, out_row, true);
		break;
	}
}

} // namespace duckdb
