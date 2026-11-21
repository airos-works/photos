// A thin layer over SQLite: statements with typed binds and columns, and
// transactions that roll back when not committed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace photos {

class Database;

class Statement {
public:
	Statement();
	Statement(Statement&& other);
	Statement& operator=(Statement&& other);
	~Statement();

	bool IsValid() const { return fStatement != nullptr; }

	// Binds are positional from 1; each returns *this for chaining.
	Statement& Bind(int index, int64_t value);
	Statement& Bind(int index, int value) { return Bind(index, (int64_t)value); }
	Statement& Bind(int index, double value);
	Statement& Bind(int index, const std::string& value);
	Statement& Bind(int index, const char* value);
	Statement& BindBlob(int index, const void* data, size_t size);
	Statement& BindNull(int index);

	bool Step();			// true while there is a row
	bool Run();				// for statements without rows: true on success
	void Reset();

	int64_t Int(int column) const;
	double Double(int column) const;
	std::string Text(int column) const;
	bool IsNull(int column) const;
	const void* Blob(int column, size_t& size) const;

private:
	friend class Database;
	Statement(const Statement&) = delete;
	Statement& operator=(const Statement&) = delete;

	sqlite3_stmt* fStatement;
	Database* fDatabase;
};

class Database {
public:
	Database();
	~Database();

	bool Open(const std::string& path, std::string* error = nullptr);
	void Close();
	bool IsOpen() const { return fHandle != nullptr; }

	bool Exec(const std::string& sql);
	Statement Prepare(const std::string& sql);
	int64_t LastInsertId() const;
	int Changes() const;
	const std::string& LastError() const { return fError; }

	bool Begin();
	bool Commit();
	void Rollback();

private:
	friend class Statement;
	void _Remember(int code);

	sqlite3* fHandle;
	std::string fError;
	int fDepth;
};

// Begins on construction (nested ones join the outer transaction), rolls back
// on destruction unless Commit() was called.
class Transaction {
public:
	explicit Transaction(Database& database);
	~Transaction();
	bool Commit();

private:
	Database& fDatabase;
	bool fDone;
};

}	// namespace photos
