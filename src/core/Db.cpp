#include "Db.h"

#include <sqlite3.h>

#include <cstdio>

namespace photos {

Statement::Statement()
	:
	fStatement(nullptr),
	fDatabase(nullptr)
{
}


Statement::Statement(Statement&& other)
	:
	fStatement(other.fStatement),
	fDatabase(other.fDatabase)
{
	other.fStatement = nullptr;
}


Statement&
Statement::operator=(Statement&& other)
{
	if (this != &other) {
		if (fStatement != nullptr)
			sqlite3_finalize(fStatement);
		fStatement = other.fStatement;
		fDatabase = other.fDatabase;
		other.fStatement = nullptr;
	}
	return *this;
}


Statement::~Statement()
{
	if (fStatement != nullptr)
		sqlite3_finalize(fStatement);
}


Statement&
Statement::Bind(int index, int64_t value)
{
	if (fStatement != nullptr)
		sqlite3_bind_int64(fStatement, index, value);
	return *this;
}


Statement&
Statement::Bind(int index, double value)
{
	if (fStatement != nullptr)
		sqlite3_bind_double(fStatement, index, value);
	return *this;
}


Statement&
Statement::Bind(int index, const std::string& value)
{
	if (fStatement != nullptr) {
		sqlite3_bind_text(fStatement, index, value.data(), (int)value.size(),
			SQLITE_TRANSIENT);
	}
	return *this;
}


Statement&
Statement::Bind(int index, const char* value)
{
	if (fStatement != nullptr) {
		if (value == nullptr)
			sqlite3_bind_null(fStatement, index);
		else
			sqlite3_bind_text(fStatement, index, value, -1, SQLITE_TRANSIENT);
	}
	return *this;
}


Statement&
Statement::BindBlob(int index, const void* data, size_t size)
{
	if (fStatement != nullptr) {
		sqlite3_bind_blob64(fStatement, index, data, size, SQLITE_TRANSIENT);
	}
	return *this;
}


Statement&
Statement::BindNull(int index)
{
	if (fStatement != nullptr)
		sqlite3_bind_null(fStatement, index);
	return *this;
}


bool
Statement::Step()
{
	if (fStatement == nullptr)
		return false;
	int code = sqlite3_step(fStatement);
	if (code == SQLITE_ROW)
		return true;
	if (code != SQLITE_DONE && fDatabase != nullptr)
		fDatabase->_Remember(code);
	return false;
}


bool
Statement::Run()
{
	if (fStatement == nullptr)
		return false;
	int code = sqlite3_step(fStatement);
	sqlite3_reset(fStatement);
	if (code == SQLITE_DONE || code == SQLITE_ROW)
		return true;
	if (fDatabase != nullptr)
		fDatabase->_Remember(code);
	return false;
}


void
Statement::Reset()
{
	if (fStatement != nullptr) {
		sqlite3_reset(fStatement);
		sqlite3_clear_bindings(fStatement);
	}
}


int64_t
Statement::Int(int column) const
{
	return fStatement != nullptr ? sqlite3_column_int64(fStatement, column) : 0;
}


double
Statement::Double(int column) const
{
	return fStatement != nullptr ? sqlite3_column_double(fStatement, column) : 0;
}


std::string
Statement::Text(int column) const
{
	if (fStatement == nullptr)
		return std::string();
	const unsigned char* text = sqlite3_column_text(fStatement, column);
	if (text == nullptr)
		return std::string();
	return std::string((const char*)text, sqlite3_column_bytes(fStatement, column));
}


bool
Statement::IsNull(int column) const
{
	return fStatement == nullptr
		|| sqlite3_column_type(fStatement, column) == SQLITE_NULL;
}


const void*
Statement::Blob(int column, size_t& size) const
{
	size = 0;
	if (fStatement == nullptr)
		return nullptr;
	const void* data = sqlite3_column_blob(fStatement, column);
	size = (size_t)sqlite3_column_bytes(fStatement, column);
	return data;
}


// --- Database ---------------------------------------------------------------

Database::Database()
	:
	fHandle(nullptr),
	fDepth(0)
{
}


Database::~Database()
{
	Close();
}


bool
Database::Open(const std::string& path, std::string* error)
{
	Close();
	int code = sqlite3_open_v2(path.c_str(), &fHandle,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
	if (code != SQLITE_OK) {
		fError = fHandle != nullptr ? sqlite3_errmsg(fHandle) : "out of memory";
		if (error != nullptr)
			*error = fError;
		Close();
		return false;
	}
	sqlite3_busy_timeout(fHandle, 10000);
	// A journal in the same directory, kept: the library may live on a volume
	// where shared memory files (WAL) are not possible.
	Exec("PRAGMA journal_mode=TRUNCATE");
	Exec("PRAGMA synchronous=NORMAL");
	Exec("PRAGMA foreign_keys=ON");
	Exec("PRAGMA temp_store=MEMORY");
	return true;
}


void
Database::Close()
{
	if (fHandle != nullptr) {
		sqlite3_close(fHandle);
		fHandle = nullptr;
	}
	fDepth = 0;
}


void
Database::_Remember(int code)
{
	fError = fHandle != nullptr ? sqlite3_errmsg(fHandle) : "closed";
	fprintf(stderr, "photos: database: %s (%d)\n", fError.c_str(), code);
}


bool
Database::Exec(const std::string& sql)
{
	if (fHandle == nullptr)
		return false;
	char* message = nullptr;
	int code = sqlite3_exec(fHandle, sql.c_str(), nullptr, nullptr, &message);
	if (code != SQLITE_OK) {
		fError = message != nullptr ? message : "error";
		fprintf(stderr, "photos: database: %s: %s\n", fError.c_str(),
			sql.substr(0, 120).c_str());
		sqlite3_free(message);
		return false;
	}
	return true;
}


Statement
Database::Prepare(const std::string& sql)
{
	Statement statement;
	if (fHandle == nullptr)
		return statement;
	statement.fDatabase = this;
	int code = sqlite3_prepare_v2(fHandle, sql.c_str(), (int)sql.size(),
		&statement.fStatement, nullptr);
	if (code != SQLITE_OK) {
		_Remember(code);
		fprintf(stderr, "photos: in: %s\n", sql.substr(0, 160).c_str());
		statement.fStatement = nullptr;
	}
	return statement;
}


int64_t
Database::LastInsertId() const
{
	return fHandle != nullptr ? sqlite3_last_insert_rowid(fHandle) : 0;
}


int
Database::Changes() const
{
	return fHandle != nullptr ? sqlite3_changes(fHandle) : 0;
}


bool
Database::Begin()
{
	if (fDepth++ > 0)
		return true;
	if (!Exec("BEGIN IMMEDIATE")) {
		fDepth = 0;
		return false;
	}
	return true;
}


bool
Database::Commit()
{
	if (fDepth == 0)
		return false;
	if (--fDepth > 0)
		return true;
	return Exec("COMMIT");
}


void
Database::Rollback()
{
	if (fDepth == 0)
		return;
	fDepth = 0;
	Exec("ROLLBACK");
}


Transaction::Transaction(Database& database)
	:
	fDatabase(database),
	fDone(false)
{
	fDatabase.Begin();
}


Transaction::~Transaction()
{
	if (!fDone)
		fDatabase.Rollback();
}


bool
Transaction::Commit()
{
	fDone = true;
	return fDatabase.Commit();
}

}	// namespace photos
