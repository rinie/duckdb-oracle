#include "duckdb.hpp"
#include <dpi.h>
#include "duckdb/parser/parsed_data/create_table_info.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "oracle_filter_pushdown.hpp"
#include "oracle_scanner.hpp"
#include "oracle_result.hpp"
#include "oracle_conversion.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_set.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// OracleReader - streaming row reader using ODPI-C
// ---------------------------------------------------------------------------

enum class OracleReadResult { CHUNK_DONE, FINISHED };

class OracleReader {
public:
	OracleReader(OracleConnection &conn, const vector<column_t> &column_ids,
	              const OracleBindData &bind_data, vector<string> *rowid_registry = nullptr)
	    : connection(conn), column_ids(column_ids), bind_data(bind_data),
	      rowid_registry_(rowid_registry) {
	}

	~OracleReader() {
		if (stmt) {
			dpiStmt_release(stmt);
			stmt = nullptr;
		}
	}

	void BeginQuery(optional_ptr<ClientContext> context, const string &sql) {
		if (stmt) {
			dpiStmt_release(stmt);
			stmt = nullptr;
		}
		done = false;
		auto conn = connection.GetConn();
		dpiErrorInfo error_info;
		if (dpiConn_prepareStmt(conn, 0, sql.c_str(), (uint32_t)sql.size(),
		                         nullptr, 0, &stmt) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			throw IOException("Oracle prepare failed: %s", string(error_info.message));
		}
		// Set fetch array size for bulk fetching
		dpiStmt_setFetchArraySize(stmt, FETCH_ARRAY_SIZE);

		if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &num_cols) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			stmt = nullptr;
			throw IOException("Oracle execute failed: %s (query: %s)",
			                  string(error_info.message), sql);
		}

		// Cache query info for each column
		col_info.resize(num_cols);
		for (uint32_t i = 0; i < num_cols; i++) {
			if (dpiStmt_getQueryInfo(stmt, i + 1, &col_info[i]) < 0) {
				dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
				throw IOException("Oracle getQueryInfo failed: %s", string(error_info.message));
			}
		}
	}

	OracleReadResult Read(DataChunk &output) {
		if (done || !stmt) {
			return OracleReadResult::FINISHED;
		}
		auto conn = connection.GetConn();
		dpiErrorInfo error_info;
		idx_t output_offset = 0;
		auto max_rows = STANDARD_VECTOR_SIZE;

		while (output_offset < (idx_t)max_rows) {
			int found = 0;
			uint32_t buffer_row_index = 0;
			if (dpiStmt_fetch(stmt, &found, &buffer_row_index) < 0) {
				dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
				throw IOException("Oracle fetch failed: %s", string(error_info.message));
			}
			if (!found) {
				done = true;
				break;
			}

			// Convert each projected column
			for (idx_t proj_idx = 0; proj_idx < column_ids.size(); proj_idx++) {
				auto col_id = column_ids[proj_idx];
				auto &out_col = output.data[proj_idx];

				if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
					if (rowid_registry_) {
						uint32_t query_col = (uint32_t)(proj_idx + 1);
						dpiNativeTypeNum native_type;
						dpiData *data;
						if (dpiStmt_getQueryValue(stmt, query_col, &native_type, &data) >= 0 &&
						    !data->isNull && native_type == DPI_NATIVE_TYPE_ROWID) {
							const char *ptr = nullptr;
							uint32_t len = 0;
							if (dpiRowid_getStringValue(data->value.asRowid, &ptr, &len) == 0 && ptr) {
								int64_t idx = (int64_t)rowid_registry_->size();
								rowid_registry_->emplace_back(ptr, len);
								FlatVector::GetDataMutable<int64_t>(out_col)[output_offset] = idx;
								continue;
							}
						}
					}
					FlatVector::SetNull(out_col, output_offset, true);
					continue;
				}

				// Query column index (1-based for ODPI-C)
				// The query was built with projected columns in order
				uint32_t query_col = (uint32_t)(proj_idx + 1);

				dpiNativeTypeNum native_type;
				dpiData *data;
				if (dpiStmt_getQueryValue(stmt, query_col, &native_type, &data) < 0) {
					dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
					throw IOException("Oracle getQueryValue failed: %s",
					                  string(error_info.message));
				}

				OracleConvertValue(out_col, output_offset, data, native_type,
				                    bind_data.types[col_id],
				                    bind_data.oracle_types[col_id]);
			}
			output_offset++;
		}
		output.SetCardinality(output_offset);
		if (done) {
			return OracleReadResult::FINISHED;
		}
		return OracleReadResult::CHUNK_DONE;
	}

private:
	OracleConnection &connection;
	const vector<column_t> &column_ids;
	const OracleBindData &bind_data;
	vector<string> *rowid_registry_;
	dpiStmt *stmt = nullptr;
	uint32_t num_cols = 0;
	vector<dpiQueryInfo> col_info;
	bool done = false;
	static constexpr uint32_t FETCH_ARRAY_SIZE = 2000;
};

// ---------------------------------------------------------------------------
// Global / Local state
// ---------------------------------------------------------------------------

struct OracleGlobalState;

struct OracleLocalState : public LocalTableFunctionState {
	bool done = false;
	bool exec = false;
	bool no_connection = false;
	string sql;
	vector<column_t> column_ids;
	TableFilterSet *filters = nullptr;
	OracleConnection connection;
	OraclePoolConnection pool_connection;
	unique_ptr<OracleReader> reader;
	// Set during UPDATE/DELETE materialization to collect Oracle ROWID strings.
	vector<string> *rowid_registry = nullptr;

	void ScanChunk(ClientContext &context, const OracleBindData &bind_data,
	               OracleGlobalState &gstate, DataChunk &output);
};

struct OracleGlobalState : public GlobalTableFunctionState {
	explicit OracleGlobalState(idx_t max_threads_p)
	    : max_threads(max_threads_p), used_main_thread(false) {
	}

	mutable mutex lock;
	idx_t max_threads;
	bool used_main_thread;
	unique_ptr<ColumnDataCollection> collection;
	ColumnDataScanState scan_state;

	OracleConnection &GetConnection() {
		return connection;
	}
	void SetConnection(OracleConnection conn) {
		connection = std::move(conn);
	}
	void SetConnection(shared_ptr<OwnedOracleConnection> conn) {
		connection = OracleConnection(std::move(conn));
	}

	bool TryOpenNewConnection(ClientContext &context, OracleLocalState &lstate,
	                           const OracleBindData &bind_data);

	idx_t MaxThreads() const override {
		return max_threads;
	}

private:
	OracleConnection connection;
};

// ---------------------------------------------------------------------------
// OracleBindData helpers
// ---------------------------------------------------------------------------

OracleBindData::OracleBindData() {
}

void OracleBindData::SetCatalog(OracleCatalog &catalog) {
	this->ora_catalog = &catalog;
}

void OracleBindData::SetTable(OracleTableEntry &table) {
	this->ora_table = &table;
}

// ---------------------------------------------------------------------------
// Build SELECT query for the scan
// ---------------------------------------------------------------------------

static void OracleInitInternal(ClientContext &context, const OracleBindData &bind_data,
                                OracleLocalState &lstate) {
	// Use oracle_names (original Oracle data-dictionary case) for SQL generation.
	// bind_data.names may have been lowercased by DuckDB's catalog pipeline; quoting
	// a lowercased name like "emp_id" would fail because Oracle stores it as EMP_ID.
	// oracle_names always carries the exact case returned by all_tab_columns.
	const auto &sql_names =
	    bind_data.oracle_names.empty() ? bind_data.names : bind_data.oracle_names;

	string col_names;
	for (auto &column_id : lstate.column_ids) {
		if (!col_names.empty()) {
			col_names += ", ";
		}
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			col_names += "ROWID";
		} else {
			col_names += OracleUtils::QuoteIdentifier(sql_names[column_id]);
		}
	}

	string filter_string = OracleFilterPushdown::TransformFilters(
	    lstate.column_ids, lstate.filters, sql_names);

	string query;
	if (bind_data.table_name.empty()) {
		D_ASSERT(!bind_data.sql.empty());
		query = StringUtil::Format("SELECT %s FROM (%s) __ora_sub", col_names,
		                           bind_data.sql);
	} else {
		query = StringUtil::Format("SELECT %s FROM %s.%s", col_names,
		                           OracleUtils::QuoteIdentifier(bind_data.schema_name),
		                           OracleUtils::QuoteIdentifier(bind_data.table_name));
	}

	if (!filter_string.empty()) {
		query += " WHERE " + filter_string;
	}
	if (!bind_data.limit.empty()) {
		query += " " + bind_data.limit;
	}

	lstate.done = false;
	lstate.exec = false;
	lstate.sql = std::move(query);
}

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> OracleBind(ClientContext &context,
                                            TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types,
                                            vector<string> &names) {
	auto bind_data = make_uniq<OracleBindData>();

	bind_data->dsn = input.inputs[0].GetValue<string>();
	bind_data->schema_name = input.inputs[1].GetValue<string>();
	bind_data->table_name = input.inputs[2].GetValue<string>();
	bind_data->attach_path = bind_data->dsn;

	auto con = OracleConnection::Open(bind_data->dsn, bind_data->attach_path);
	bind_data->version = con.GetOracleVersion();

	auto info = OracleTableSet::GetTableInfo(context, con, bind_data->schema_name,
	                                          bind_data->table_name);

	bind_data->oracle_types = info->oracle_types;
	for (auto &col : info->create_info->columns.Logical()) {
		names.push_back(col.GetName());
		return_types.push_back(col.GetType());
	}
	bind_data->names = info->oracle_names;
	bind_data->types = return_types;
	bind_data->can_use_main_thread = true;
	bind_data->requires_materialization = false;

	return std::move(bind_data);
}

// ---------------------------------------------------------------------------
// InitGlobalState / InitLocalState
// ---------------------------------------------------------------------------

static unique_ptr<LocalTableFunctionState> GetLocalState(ClientContext &context,
                                                          TableFunctionInitInput &input,
                                                          OracleGlobalState &gstate);

static void OracleScanConnect(ClientContext &context, OracleConnection &conn) {
	// Oracle: transactions start automatically on first DML.
	// For read queries we do not need an explicit BEGIN.
	// Set read-only session is done at catalog level.
}

static unique_ptr<GlobalTableFunctionState>
OracleInitGlobalState(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<OracleBindData>();
	auto result = make_uniq<OracleGlobalState>(1); // single-threaded for now
	auto ora_catalog = bind_data.GetCatalog();
	if (ora_catalog) {
		auto &transaction = Transaction::Get(context, *ora_catalog).Cast<OracleTransaction>();
		auto &con = transaction.GetConnection();
		result->SetConnection(con.GetConnection());
	} else {
		auto con = OracleConnection::Open(bind_data.dsn, bind_data.attach_path);
		result->SetConnection(std::move(con));
	}

	if (bind_data.requires_materialization) {
		vector<LogicalType> types;
		for (auto col_id : input.column_ids) {
			types.push_back(col_id == COLUMN_IDENTIFIER_ROW_ID ? LogicalType::BIGINT
			                                                    : bind_data.types[col_id]);
		}
		auto materialized = make_uniq<ColumnDataCollection>(Allocator::Get(context), types);
		DataChunk scan_chunk;
		scan_chunk.Initialize(Allocator::Get(context), types);

		auto local_state = GetLocalState(context, input, *result);
		auto &lstate = local_state->Cast<OracleLocalState>();

		// Wire up the rowid registry if this scan projects COLUMN_IDENTIFIER_ROW_ID
		// (used for UPDATE/DELETE to map BIGINT indices → Oracle ROWID strings).
		if (ora_catalog) {
			bool has_rowid = false;
			for (auto col_id : input.column_ids) {
				if (col_id == COLUMN_IDENTIFIER_ROW_ID) { has_rowid = true; break; }
			}
			if (has_rowid) {
				auto &transaction = Transaction::Get(context, *ora_catalog).Cast<OracleTransaction>();
				transaction.ClearRowidRegistry();
				lstate.rowid_registry = &transaction.rowid_registry;
			}
		}
		ColumnDataAppendState append_state;
		materialized->InitializeAppend(append_state);
		while (true) {
			scan_chunk.Reset();
			lstate.ScanChunk(context, bind_data, *result, scan_chunk);
			if (scan_chunk.size() == 0) {
				break;
			}
			materialized->Append(append_state, scan_chunk);
		}
		result->collection = std::move(materialized);
		result->collection->InitializeScan(result->scan_state);
	}
	return std::move(result);
}

bool OracleGlobalState::TryOpenNewConnection(ClientContext &context,
                                              OracleLocalState &lstate,
                                              const OracleBindData &bind_data) {
	auto ora_catalog = bind_data.GetCatalog();
	lock_guard<mutex> parallel_lock(lock);
	if (!used_main_thread) {
		if (bind_data.can_use_main_thread) {
			lstate.connection = OracleConnection(GetConnection().GetConnection());
		} else {
			lstate.pool_connection = ora_catalog->GetConnectionPool().ForceGetConnection();
			lstate.connection =
			    OracleConnection(lstate.pool_connection.GetConnection().GetConnection());
		}
		used_main_thread = true;
		return true;
	}
	if (ora_catalog) {
		if (!ora_catalog->GetConnectionPool().TryGetConnection(lstate.pool_connection)) {
			return false;
		}
		lstate.connection =
		    OracleConnection(lstate.pool_connection.GetConnection().GetConnection());
	} else {
		lstate.connection =
		    OracleConnection::Open(bind_data.dsn, bind_data.attach_path);
	}
	return true;
}

static unique_ptr<LocalTableFunctionState>
GetLocalState(ClientContext &context, TableFunctionInitInput &input,
              OracleGlobalState &gstate) {
	auto &bind_data = (OracleBindData &)*input.bind_data;
	auto local_state = make_uniq<OracleLocalState>();
	if (gstate.collection) {
		return std::move(local_state);
	}
	local_state->column_ids = input.column_ids;
	local_state->filters = input.filters.get();
	if (!gstate.TryOpenNewConnection(context, *local_state, bind_data)) {
		local_state->no_connection = true;
		return std::move(local_state);
	}
	OracleInitInternal(context, bind_data, *local_state);
	return std::move(local_state);
}

static unique_ptr<LocalTableFunctionState>
OracleInitLocalState(ExecutionContext &context, TableFunctionInitInput &input,
                      GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<OracleGlobalState>();
	return GetLocalState(context.client, input, gstate);
}

// ---------------------------------------------------------------------------
// ScanChunk / Scan
// ---------------------------------------------------------------------------

void OracleLocalState::ScanChunk(ClientContext &context, const OracleBindData &bind_data,
                                   OracleGlobalState &gstate, DataChunk &output) {
	if (done) {
		return;
	}
	if (!reader) {
		reader = make_uniq<OracleReader>(connection, column_ids, bind_data, rowid_registry);
	}
	if (!exec) {
		reader->BeginQuery(context, sql);
		exec = true;
	}
	auto read_result = reader->Read(output);
	if (read_result == OracleReadResult::FINISHED) {
		done = true;
	}
}

static void OracleScan(ClientContext &context, TableFunctionInput &data,
                        DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<OracleBindData>();
	auto &gstate = data.global_state->Cast<OracleGlobalState>();

	if (gstate.collection) {
		gstate.collection->Scan(gstate.scan_state, output);
		return;
	}
	auto &local_state = data.local_state->Cast<OracleLocalState>();
	if (local_state.no_connection) {
		return;
	}
	local_state.ScanChunk(context, bind_data, gstate, output);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static InsertionOrderPreservingMap<string>
OracleScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<OracleBindData>();
	result["Table"] = bind_data.table_name;
	return result;
}

static void OracleScanSerialize(Serializer &, const optional_ptr<FunctionData>,
                                 const TableFunction &) {
	throw NotImplementedException("OracleScanSerialize");
}

static unique_ptr<FunctionData> OracleScanDeserialize(Deserializer &,
                                                       TableFunction &) {
	throw NotImplementedException("OracleScanDeserialize");
}

static BindInfo OracleGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<OracleBindData>();
	BindInfo info(ScanType::EXTERNAL);
	info.table = bind_data.GetTable().get();
	return info;
}

// ---------------------------------------------------------------------------
// Function registration
// ---------------------------------------------------------------------------

OracleScanFunction::OracleScanFunction()
    : TableFunction("oracle_scan",
                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    OracleScan, OracleBind, OracleInitGlobalState,
                    OracleInitLocalState) {
	to_string = OracleScanToString;
	serialize = OracleScanSerialize;
	deserialize = OracleScanDeserialize;
	get_bind_info = OracleGetBindInfo;
	projection_pushdown = true;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}

OracleScanFunctionFilterPushdown::OracleScanFunctionFilterPushdown()
    : TableFunction("oracle_scan_pushdown",
                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    OracleScan, OracleBind, OracleInitGlobalState,
                    OracleInitLocalState) {
	to_string = OracleScanToString;
	serialize = OracleScanSerialize;
	deserialize = OracleScanDeserialize;
	get_bind_info = OracleGetBindInfo;
	projection_pushdown = true;
	filter_pushdown = true;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}

void OracleScanFunction::PrepareBind(OracleVersion version, ClientContext &context,
                                      OracleBindData &bind_data,
                                      idx_t approx_num_rows) {
	bind_data.version = version;
	bind_data.max_threads = 1; // single-threaded; ROWID-based parallel is future work
}

} // namespace duckdb
