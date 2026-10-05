#include "sqlite_integration.h"

#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define HAS_SQLITE3 1
#else
#define HAS_SQLITE3 0
#endif

namespace {
#if HAS_SQLITE3
sqlite3* g_db = nullptr;

void exec_or_throw(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

int pseudo_page(int key, int bucket) {
    return (key * 17 + bucket * 31) % 200;
}
#else
std::vector<int> synthetic_trace(const std::string& workload) {
    std::vector<int> trace;
    if (workload == "SELECT_SCAN") {
        for (int i = 0; i < 200; ++i) trace.push_back((i * 3) % 128);
    } else if (workload == "SELECT_POINT") {
        for (int i = 0; i < 150; ++i) trace.push_back(((i * 29) % 37) + 10);
    } else if (workload == "JOIN") {
        for (int i = 0; i < 220; ++i) {
            trace.push_back((i * 5) % 90);
            trace.push_back((i * 7) % 90);
        }
    } else if (workload == "RANGE") {
        for (int i = 0; i < 180; ++i) {
            trace.push_back((i * 11) % 110);
            trace.push_back((i * 11 + 1) % 110);
        }
    }
    return trace;
}
#endif
}  // namespace

void init_sqlite_db(const std::string& db_path) {
#if HAS_SQLITE3
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }

    if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
        throw std::runtime_error("Failed to open simulation.db");
    }

    exec_or_throw(g_db, "DROP TABLE IF EXISTS Students;");
    exec_or_throw(g_db, "DROP TABLE IF EXISTS Courses;");
    exec_or_throw(
        g_db,
        "CREATE TABLE Students (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, dept TEXT);");
    exec_or_throw(
        g_db,
        "CREATE TABLE Courses (id INTEGER PRIMARY KEY, student_id INTEGER, course TEXT, grade INTEGER);");

    exec_or_throw(g_db, "BEGIN TRANSACTION;");
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> age_dist(18, 27);
    std::uniform_int_distribution<int> dept_dist(0, 3);
    std::uniform_int_distribution<int> sid_dist(1, 200);
    std::uniform_int_distribution<int> grade_dist(50, 100);
    const char* depts[] = {"CS", "EE", "ME", "CE"};

    sqlite3_stmt* student_stmt = nullptr;
    sqlite3_prepare_v2(
        g_db,
        "INSERT INTO Students(id, name, age, dept) VALUES(?, ?, ?, ?);",
        -1,
        &student_stmt,
        nullptr);
    for (int i = 1; i <= 200; ++i) {
        std::string name = "Student_" + std::to_string(i);
        sqlite3_bind_int(student_stmt, 1, i);
        sqlite3_bind_text(student_stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(student_stmt, 3, age_dist(rng));
        sqlite3_bind_text(student_stmt, 4, depts[dept_dist(rng)], -1, SQLITE_TRANSIENT);
        sqlite3_step(student_stmt);
        sqlite3_reset(student_stmt);
        sqlite3_clear_bindings(student_stmt);
    }
    sqlite3_finalize(student_stmt);

    sqlite3_stmt* course_stmt = nullptr;
    sqlite3_prepare_v2(
        g_db,
        "INSERT INTO Courses(id, student_id, course, grade) VALUES(?, ?, ?, ?);",
        -1,
        &course_stmt,
        nullptr);
    for (int i = 1; i <= 100; ++i) {
        std::string course = "Course_" + std::to_string((i % 10) + 1);
        sqlite3_bind_int(course_stmt, 1, i);
        sqlite3_bind_int(course_stmt, 2, sid_dist(rng));
        sqlite3_bind_text(course_stmt, 3, course.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(course_stmt, 4, grade_dist(rng));
        sqlite3_step(course_stmt);
        sqlite3_reset(course_stmt);
        sqlite3_clear_bindings(course_stmt);
    }
    sqlite3_finalize(course_stmt);
    exec_or_throw(g_db, "COMMIT;");
#else
    (void)db_path;
#endif
}

std::vector<int> get_page_trace(const std::string& db_path, const std::string& workload) {
#if HAS_SQLITE3
    if (!g_db) {
        if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
            return {};
        }
    }

    std::vector<int> trace;
    sqlite3_stmt* stmt = nullptr;

    if (workload == "SELECT_SCAN") {
        sqlite3_prepare_v2(g_db, "SELECT id FROM Students ORDER BY id;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int id = sqlite3_column_int(stmt, 0);
            trace.push_back(pseudo_page(id, 1));
        }
    } else if (workload == "SELECT_POINT") {
        sqlite3_prepare_v2(g_db, "SELECT id FROM Students WHERE id = ?;", -1, &stmt, nullptr);
        for (int i = 1; i <= 150; ++i) {
            const int lookup = ((i * 13) % 200) + 1;
            sqlite3_bind_int(stmt, 1, lookup);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                trace.push_back(pseudo_page(sqlite3_column_int(stmt, 0), 2));
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    } else if (workload == "JOIN") {
        sqlite3_prepare_v2(
            g_db,
            "SELECT s.id, c.id FROM Students s JOIN Courses c ON s.id = c.student_id;",
            -1,
            &stmt,
            nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int sid = sqlite3_column_int(stmt, 0);
            const int cid = sqlite3_column_int(stmt, 1);
            trace.push_back(pseudo_page(sid, 3));
            trace.push_back(pseudo_page(cid, 4));
        }
    } else if (workload == "RANGE") {
        sqlite3_prepare_v2(
            g_db,
            "SELECT id FROM Students WHERE age BETWEEN 20 AND 24 ORDER BY id;",
            -1,
            &stmt,
            nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int id = sqlite3_column_int(stmt, 0);
            trace.push_back(pseudo_page(id, 5));
            trace.push_back(pseudo_page(id + 1, 6));
        }
    }

    if (stmt) {
        sqlite3_finalize(stmt);
    }
    return trace;
#else
    (void)db_path;
    return synthetic_trace(workload);
#endif
}

QueryExecutionResult execute_sql_query(const std::string& db_path, const std::string& sql) {
    QueryExecutionResult result;
#if HAS_SQLITE3
    if (!g_db) {
        if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
            result.error = "Failed to open SQLite database";
            return result;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(g_db);
        return result;
    }

    const int col_count = sqlite3_column_count(stmt);
    for (int i = 0; i < col_count; ++i) {
        const char* col_name = sqlite3_column_name(stmt, i);
        result.columns.push_back(col_name ? col_name : ("col_" + std::to_string(i)));
    }

    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            result.error = sqlite3_errmsg(g_db);
            break;
        }
        std::vector<std::string> row;
        row.reserve(col_count);
        for (int i = 0; i < col_count; ++i) {
            const unsigned char* txt = sqlite3_column_text(stmt, i);
            row.push_back(txt ? reinterpret_cast<const char*>(txt) : "NULL");
        }
        result.rows.push_back(row);
    }
    sqlite3_finalize(stmt);
#else
    (void)db_path;
    result.columns = {"message"};
    result.rows = {{"SQLite headers not available; running in synthetic mode."},
                   {"Query received: " + sql}};
#endif
    return result;
}

void close_sqlite_db() {
#if HAS_SQLITE3
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
#endif
}
