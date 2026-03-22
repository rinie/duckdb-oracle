#include "storage/oracle_schema_entry.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_set.hpp"
#include "storage/oracle_index_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

OracleSchemaEntry::OracleSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), oracle_name(name), tables(*this), indexes(*this),
      types(*this) {
}

// Single source of truth for Oracle internal schema names.
static const vector<string> &OracleInternalSchemaNames() {
	static const vector<string> schemas = {
	    "SYS", "SYSTEM", "OUTLN", "XDB", "MDSYS", "CTXSYS", "DBSNMP", "APPQOSSYS",
	    "OJVMSYS", "GSMADMIN_INTERNAL", "ORDDATA", "ORDSYS", "SI_INFORMTN_SCHEMA"};
	return schemas;
}

bool OracleSchemaEntry::SchemaIsInternal(const string &name) {
	for (auto &s : OracleInternalSchemaNames()) {
		if (StringUtil::CIEquals(name, s)) {
			return true;
		}
	}
	return false;
}

string OracleSchemaEntry::InternalOwnersSQL() {
	// Build the SQL literal list once, e.g. "'SYS','SYSTEM',..."
	static const string sql = []() {
		string out;
		for (auto &s : OracleInternalSchemaNames()) {
			if (!out.empty()) {
				out += ",";
			}
			out += "'" + s + "'";
		}
		return out;
	}();
	return sql;
}

bool OracleSchemaEntry::IsCurrentUserSchema() const {
	// Use oracle_name (not name) so the "main" alias schema also benefits from
	// USER_* views — oracle_name equals the connected user for both the real
	// schema and the "main" alias that points at it.
	return StringUtil::CIEquals(oracle_name,
	                             ParentCatalog().Cast<OracleCatalog>().GetDefaultSchema());
}

OracleCatalogSet &OracleSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return tables;
	case CatalogType::INDEX_ENTRY:
		return indexes;
	case CatalogType::TYPE_ENTRY:
		return types;
	default:
		throw NotImplementedException("OracleSchemaEntry::GetCatalogSet - unsupported type %s",
		                               CatalogTypeToString(type));
	}
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), catalog);
	return tables.CreateTable(oracle_transaction, info);
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                               CreateFunctionInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateFunction");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateIndex(CatalogTransaction transaction,
                                                            CreateIndexInfo &info,
                                                            TableCatalogEntry &table) {
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), catalog);
	// Issue a CREATE INDEX statement
	string sql = "CREATE ";
	if (info.constraint_type == IndexConstraintType::UNIQUE) {
		sql += "UNIQUE ";
	}
	sql += "INDEX " + OracleUtils::QuoteIdentifier(info.index_name) + " ON ";
	sql += OracleUtils::QuoteIdentifier(this->oracle_name) + "." +
	       OracleUtils::QuoteIdentifier(info.table);
	sql += "(";
	for (idx_t i = 0; i < info.column_ids.size(); i++) {
		if (i > 0) sql += ", ";
		sql += OracleUtils::QuoteIdentifier(
		    table.GetColumn(LogicalIndex(info.column_ids[i])).Name());
	}
	sql += ")";
	oracle_transaction.Query(sql);
	// Return a placeholder entry
	auto entry = make_shared_ptr<OracleIndexEntry>(
	    static_cast<Catalog &>(catalog), static_cast<SchemaCatalogEntry &>(*this), info);
	return indexes.CreateEntry(oracle_transaction, std::move(entry));
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateView(CatalogTransaction transaction,
                                                           CreateViewInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateView - use oracle_execute for DDL");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                               CreateSequenceInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateSequence");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateTableFunction(
    CatalogTransaction transaction, CreateTableFunctionInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateTableFunction");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateCopyFunction(
    CatalogTransaction transaction, CreateCopyFunctionInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateCopyFunction");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreatePragmaFunction(
    CatalogTransaction transaction, CreatePragmaFunctionInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreatePragmaFunction");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateCollation(
    CatalogTransaction transaction, CreateCollationInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateCollation");
}

optional_ptr<CatalogEntry> OracleSchemaEntry::CreateType(CatalogTransaction transaction,
                                                           CreateTypeInfo &info) {
	throw NotImplementedException("OracleSchemaEntry::CreateType");
}

void OracleSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), catalog);
	if (info.type == AlterType::ALTER_TABLE) {
		auto &alter = info.Cast<AlterTableInfo>();
		tables.AlterTable(transaction.GetContext(), oracle_transaction, alter);
	} else {
		throw NotImplementedException(
		    "OracleSchemaEntry::Alter - unsupported alter type");
	}
}

void OracleSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::TYPE_ENTRY:
		break;
	default:
		// Unsupported types (e.g. SCALAR_FUNCTION_ENTRY scanned by the UI extension)
		// must be silently ignored to avoid a fatal InternalException that invalidates the DB.
		return;
	}
	auto &oracle_transaction = OracleTransaction::Get(context, catalog);
	auto &catalog_set = GetCatalogSet(type);
	catalog_set.Scan(context, oracle_transaction, callback);
}

void OracleSchemaEntry::Scan(CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("OracleSchemaEntry::Scan without context");
}

void OracleSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	TryDropEntry(context, info.type, info.name);
}

void OracleSchemaEntry::TryDropEntry(ClientContext &context, CatalogType catalog_type,
                                      const string &name) {
	auto &oracle_transaction = OracleTransaction::Get(context, catalog);
	string sql;
	switch (catalog_type) {
	case CatalogType::TABLE_ENTRY:
		sql = "DROP TABLE " + OracleUtils::QuoteIdentifier(this->oracle_name) + "." +
		      OracleUtils::QuoteIdentifier(name);
		break;
	case CatalogType::VIEW_ENTRY:
		sql = "DROP VIEW " + OracleUtils::QuoteIdentifier(this->oracle_name) + "." +
		      OracleUtils::QuoteIdentifier(name);
		break;
	case CatalogType::INDEX_ENTRY:
		sql = "DROP INDEX " + OracleUtils::QuoteIdentifier(name);
		break;
	default:
		throw NotImplementedException("OracleSchemaEntry::DropEntry - unsupported type %s",
		                               CatalogTypeToString(catalog_type));
	}
	oracle_transaction.Query(sql);
	auto &cat_set = GetCatalogSet(catalog_type);
	DropInfo drop_info;
	drop_info.type = catalog_type;
	drop_info.name = name;
	drop_info.if_not_found = OnEntryNotFound::RETURN_NULL;
	cat_set.DropEntry(oracle_transaction, drop_info);
}

optional_ptr<CatalogEntry> OracleSchemaEntry::LookupEntry(
    CatalogTransaction transaction, const EntryLookupInfo &lookup_info) {
	auto &oracle_transaction = OracleTransaction::Get(transaction.GetContext(), catalog);
	auto type = lookup_info.GetCatalogType();
	auto &name = lookup_info.GetEntryName();
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return tables.GetEntry(transaction.GetContext(), oracle_transaction, name);
	case CatalogType::INDEX_ENTRY:
		return indexes.GetEntry(transaction.GetContext(), oracle_transaction, name);
	case CatalogType::TYPE_ENTRY:
		return types.GetEntry(transaction.GetContext(), oracle_transaction, name);
	default:
		return nullptr;
	}
}

} // namespace duckdb
