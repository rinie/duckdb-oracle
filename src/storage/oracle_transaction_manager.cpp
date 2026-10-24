#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/oracle_transaction_manager.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_catalog.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

OracleTransactionManager::OracleTransactionManager(AttachedDatabase &db_p,
                                                     OracleCatalog &oracle_catalog)
    : TransactionManager(db_p), oracle_catalog(oracle_catalog) {
}

Transaction &OracleTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<OracleTransaction>(oracle_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData OracleTransactionManager::CommitTransaction(ClientContext &context,
                                                        Transaction &transaction) {
	auto &oracle_transaction = transaction.Cast<OracleTransaction>();
	oracle_transaction.Commit();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void OracleTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &oracle_transaction = transaction.Cast<OracleTransaction>();
	oracle_transaction.Rollback();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void OracleTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Oracle has no equivalent of PostgreSQL CHECKPOINT; no-op here.
}

} // namespace duckdb
