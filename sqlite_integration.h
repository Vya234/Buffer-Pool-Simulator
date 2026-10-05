#ifndef SQLITE_INTEGRATION_H
#define SQLITE_INTEGRATION_H

#include <string>
#include <vector>

struct QueryExecutionResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string error;
};

void init_sqlite_db(const std::string& db_path);
std::vector<int> get_page_trace(const std::string& db_path, const std::string& workload);
QueryExecutionResult execute_sql_query(const std::string& db_path, const std::string& sql);
void close_sqlite_db();

#endif
