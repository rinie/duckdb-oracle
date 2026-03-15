#include "storage/oracle_index.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "oracle_utils.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

IndexStorageInfo OracleIndex::GetStorageInfo(IndexStorageInfo &info) {
	return std::move(info);
}

// ---------------------------------------------------------------------------
// Physical operator: OracleCreateIndex
// Executes CREATE INDEX DDL in Oracle and registers the entry in the catalog.
// ---------------------------------------------------------------------------

class OracleCreateIndex : public PhysicalOperator {
public:
	OracleCreateIndex(PhysicalPlan &physical_plan, unique_ptr<CreateIndexInfo> info_p,
	                   TableCatalogEntry &table_p)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION,
	                        {LogicalType::BIGINT}, 1),
	      info(std::move(info_p)), table(table_p) {
	}

	unique_ptr<CreateIndexInfo> info;
	TableCatalogEntry &table;

public:
	bool IsSource() const override {
		return true;
	}

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                  OperatorSourceInput &input) const override {
		auto &catalog = table.catalog;
		auto &schema = table.schema;
		auto transaction = catalog.GetCatalogTransaction(context.client);

		// Handle conflict with existing index
		auto existing = schema.GetEntry(transaction, CatalogType::INDEX_ENTRY, info->index_name);
		if (existing) {
			switch (info->on_conflict) {
			case OnCreateConflict::IGNORE_ON_CONFLICT:
				return SourceResultType::FINISHED;
			case OnCreateConflict::ERROR_ON_CONFLICT:
				throw BinderException(
				    "Index with name \"%s\" already exists in schema \"%s\"",
				    info->index_name, schema.name);
			case OnCreateConflict::REPLACE_ON_CONFLICT: {
				DropInfo drop_info;
				drop_info.type = CatalogType::INDEX_ENTRY;
				drop_info.schema = info->schema;
				drop_info.name = info->index_name;
				schema.DropEntry(context.client, drop_info);
				break;
			}
			default:
				throw InternalException("Unsupported on_conflict for CREATE INDEX");
			}
		}

		// Delegate to schema entry which issues the Oracle DDL
		schema.CreateIndex(transaction, *info, table);
		return SourceResultType::FINISHED;
	}

	string GetName() const override {
		return "ORA_CREATE_INDEX";
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Index Name"] = info->index_name;
		result["Table Name"] = info->table;
		return result;
	}
};

// ---------------------------------------------------------------------------
// Logical extension operator: LogicalOracleCreateIndex
// ---------------------------------------------------------------------------

class LogicalOracleCreateIndex : public LogicalExtensionOperator {
public:
	LogicalOracleCreateIndex(unique_ptr<CreateIndexInfo> info_p, TableCatalogEntry &table_p)
	    : info(std::move(info_p)), table(table_p) {
	}

	unique_ptr<CreateIndexInfo> info;
	TableCatalogEntry &table;

	PhysicalOperator &CreatePlan(ClientContext &context,
	                              PhysicalPlanGenerator &planner) override {
		return planner.Make<OracleCreateIndex>(std::move(info), table);
	}

	void Serialize(Serializer &serializer) const override {
		throw NotImplementedException("Cannot serialize Oracle CREATE INDEX");
	}

	void ResolveTypes() override {
		types = {LogicalType::BIGINT};
	}
};

// ---------------------------------------------------------------------------
// OracleCatalog::BindCreateIndex
// ---------------------------------------------------------------------------

unique_ptr<LogicalOperator> OracleCatalog::BindCreateIndex(
    Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
    unique_ptr<LogicalOperator> plan) {
	auto create_index_info =
	    unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info));

	IndexBinder index_binder(binder, binder.context);
	vector<unique_ptr<Expression>> expressions;
	for (auto &expr : create_index_info->expressions) {
		expressions.push_back(index_binder.Bind(expr));
	}

	auto &get = plan->Cast<LogicalGet>();
	index_binder.InitCreateIndexInfo(get, *create_index_info, table.schema.name);

	return make_uniq<LogicalOracleCreateIndex>(std::move(create_index_info), table);
}

} // namespace duckdb
