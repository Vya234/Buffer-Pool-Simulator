#include "defs.h"
#include "sqlite_integration.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <condition_variable>
#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::vector<SimResult> g_results;
std::mutex g_results_mutex;
std::string g_active_strategy;
int g_active_buffer_size = -1;
std::unique_ptr<lru_buffer_manager> g_lru_mgr;
std::unique_ptr<mru_buffer_manager> g_mru_mgr;
std::unique_ptr<clock_buffer_manager> g_clock_mgr;

const std::vector<std::string> kStrategies = {"LRU", "MRU", "CLOCK"};
const std::vector<std::string> kQueryTypes = {"SELECT", "JOIN"};

struct StepLog {
    int step = 0;
    int page = -1;
    bool hit = false;
    int evicted_page = -1;
    bool dirty_eviction = false;
    std::string eviction_reason;
    std::vector<int> buffer_state;
    int disk_reads = 0;
    int disk_writes = 0;
};

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void add_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
}

std::string format_state(const std::vector<int>& state) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < state.size(); ++i) {
        if (state[i] == -1) {
            oss << "_";
        } else {
            oss << state[i];
        }
        if (i + 1 != state.size()) oss << ", ";
    }
    oss << "]";
    return oss.str();
}

std::string csv_line(const SimResult& r) {
    std::ostringstream oss;
    oss << r.strategy << "," << r.workload << "," << r.buffer_size << "," << r.disk_reads << "," << r.disk_writes
        << "," << r.total_io << "," << r.hits << "," << r.misses << "," << std::fixed << std::setprecision(2)
        << r.hit_rate << "," << r.evictions;
    return oss.str();
}

void append_result_csv(const SimResult& r) {
    std::ifstream in("results.csv");
    const bool exists = in.good();
    in.close();

    std::ofstream out("results.csv", std::ios::app);
    if (!exists) {
        out << "strategy,workload,buffer_size,disk_reads,disk_writes,total_io,hits,misses,hit_rate,evictions\n";
    }
    out << csv_line(r) << "\n";
}

std::string map_query_to_workload(const std::string& query_type) {
    if (query_type == "SELECT") return "SELECT_POINT";
    return "JOIN";
}

int stable_page_id_from_row(const std::vector<std::string>& row) {
    if (row.empty()) {
        return 0;
    }
    try {
        const long long v = std::stoll(row[0]);
        return static_cast<int>((v % 128 + 128) % 128);
    } catch (...) {
        unsigned long h = 5381;
        for (char c : row[0]) {
            h = ((h << 5) + h) + static_cast<unsigned char>(c);
        }
        return static_cast<int>(h % 128);
    }
}

std::vector<int> build_trace_from_query_result(
    const std::string& query_type,
    const QueryExecutionResult& query_out,
    const std::vector<int>& fallback_trace) {
    (void)query_type;
    const std::vector<int>* source = nullptr;
    std::vector<int> base_pages;

    if (!query_out.rows.empty()) {
        base_pages.reserve(query_out.rows.size());
        for (const auto& row : query_out.rows) {
            base_pages.push_back(stable_page_id_from_row(row));
        }
        source = &base_pages;
    } else {
        source = &fallback_trace;
    }

    if (!source || source->empty()) {
        return fallback_trace;
    }
    return *source;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

bool is_select_or_join_query(const std::string& sql, std::string& query_type_out) {
    const std::string t = to_lower(trim(sql));
    if (t.rfind("select", 0) != 0) {
        return false;
    }
    if (t.find(" join ") != std::string::npos) {
        query_type_out = "JOIN";
    } else {
        query_type_out = "SELECT";
    }
    return true;
}

std::string extract_json_string(const std::string& body, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t pos = body.find(token);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = body.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end = body.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return body.substr(pos + 1, end - pos - 1);
}

int extract_json_int(const std::string& body, const std::string& key, int fallback) {
    const std::string token = "\"" + key + "\"";
    size_t pos = body.find(token);
    if (pos == std::string::npos) return fallback;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    size_t end = pos;
    while (end < body.size() && (body[end] == '-' || (body[end] >= '0' && body[end] <= '9'))) ++end;
    if (end == pos) return fallback;
    return std::stoi(body.substr(pos, end - pos));
}

std::string result_json(const SimResult& r, const std::vector<StepLog>& steps) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"strategy\":\"" << escape_json(r.strategy) << "\",";
    oss << "\"workload\":\"" << escape_json(r.workload) << "\",";
    oss << "\"buffer_size\":" << r.buffer_size << ",";
    oss << "\"accesses\":" << r.accesses << ",";
    oss << "\"disk_reads\":" << r.disk_reads << ",";
    oss << "\"disk_writes\":" << r.disk_writes << ",";
    oss << "\"total_io\":" << r.total_io << ",";
    oss << "\"hits\":" << r.hits << ",";
    oss << "\"misses\":" << r.misses << ",";
    oss << "\"hit_rate\":" << std::fixed << std::setprecision(2) << r.hit_rate << ",";
    oss << "\"evictions\":" << r.evictions << ",";
    oss << "\"steps\":[";
    for (size_t i = 0; i < steps.size(); ++i) {
        const StepLog& s = steps[i];
        oss << "{";
        oss << "\"step\":" << s.step << ",";
        oss << "\"page\":" << s.page << ",";
        oss << "\"hit\":" << (s.hit ? "true" : "false") << ",";
        oss << "\"evicted_page\":" << s.evicted_page << ",";
        oss << "\"dirty_eviction\":" << (s.dirty_eviction ? "true" : "false") << ",";
        oss << "\"eviction_reason\":\"" << escape_json(s.eviction_reason) << "\",";
        oss << "\"disk_reads\":" << s.disk_reads << ",";
        oss << "\"disk_writes\":" << s.disk_writes << ",";
        oss << "\"buffer_state\":[";
        for (size_t j = 0; j < s.buffer_state.size(); ++j) {
            oss << s.buffer_state[j];
            if (j + 1 != s.buffer_state.size()) oss << ",";
        }
        oss << "]}";
        if (i + 1 != steps.size()) oss << ",";
    }
    oss << "]}";
    return oss.str();
}

std::string query_result_json(const QueryExecutionResult& qr) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"error\":\"" << escape_json(qr.error) << "\",";
    oss << "\"columns\":[";
    for (size_t i = 0; i < qr.columns.size(); ++i) {
        oss << "\"" << escape_json(qr.columns[i]) << "\"";
        if (i + 1 != qr.columns.size()) oss << ",";
    }
    oss << "],\"rows\":[";
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        oss << "[";
        for (size_t j = 0; j < qr.rows[i].size(); ++j) {
            oss << "\"" << escape_json(qr.rows[i][j]) << "\"";
            if (j + 1 != qr.rows[i].size()) oss << ",";
        }
        oss << "]";
        if (i + 1 != qr.rows.size()) oss << ",";
    }
    oss << "]}";
    return oss.str();
}

std::string run_json(const SimResult& r, const std::vector<StepLog>& steps, const QueryExecutionResult& qr) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"metrics\":" << result_json(r, steps) << ",";
    oss << "\"query_result\":" << query_result_json(qr);
    oss << "}";
    return oss.str();
}

void write_join_output(const QueryExecutionResult& qr) {
    std::ofstream out("join_output.txt", std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    for (size_t i = 0; i < qr.columns.size(); ++i) {
        out << qr.columns[i];
        if (i + 1 != qr.columns.size()) out << " | ";
    }
    out << "\n";
    for (const auto& row : qr.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            out << row[i];
            if (i + 1 != row.size()) out << " | ";
        }
        out << "\n";
    }
}

std::string results_json_array(const std::vector<SimResult>& results) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        const SimResult& r = results[i];
        oss << "{"
            << "\"strategy\":\"" << escape_json(r.strategy) << "\","
            << "\"workload\":\"" << escape_json(r.workload) << "\","
            << "\"buffer_size\":" << r.buffer_size << ","
            << "\"accesses\":" << r.accesses << ","
            << "\"disk_reads\":" << r.disk_reads << ","
            << "\"disk_writes\":" << r.disk_writes << ","
            << "\"total_io\":" << r.total_io << ","
            << "\"hits\":" << r.hits << ","
            << "\"misses\":" << r.misses << ","
            << "\"hit_rate\":" << std::fixed << std::setprecision(2) << r.hit_rate << ","
            << "\"evictions\":" << r.evictions << "}";
        if (i + 1 != results.size()) oss << ",";
    }
    oss << "]";
    return oss.str();
}

void run_trace_for_manager(
    const std::string& strategy,
    int buffer_size,
    const std::vector<int>& trace,
    bool is_write,
    SimResult& result,
    std::vector<StepLog>& steps) {
    const bool new_experiment = (g_active_strategy != strategy || g_active_buffer_size != buffer_size);
    if (new_experiment) {
        g_active_strategy = strategy;
        g_active_buffer_size = buffer_size;
        g_lru_mgr.reset();
        g_mru_mgr.reset();
        g_clock_mgr.reset();
        if (strategy == "LRU") {
            g_lru_mgr = std::make_unique<lru_buffer_manager>(buffer_size);
        } else if (strategy == "MRU") {
            g_mru_mgr = std::make_unique<mru_buffer_manager>(buffer_size);
        } else {
            g_clock_mgr = std::make_unique<clock_buffer_manager>(buffer_size);
        }
    }

    if (strategy == "LRU" && g_lru_mgr) {
        for (size_t i = 0; i < trace.size(); ++i) {
            StepInfo s;
            Page* p = g_lru_mgr->access_page(trace[i], is_write, &s);
            if (!p) continue;
            StepLog log{static_cast<int>(i + 1), trace[i], s.hit, s.evicted_page, s.dirty_eviction, s.eviction_reason,
                        s.buffer_state, s.disk_reads, s.disk_writes};
            steps.push_back(log);
            g_lru_mgr->unpin_page(p);
        }
        result.accesses = g_lru_mgr->accesses;
        result.disk_reads = g_lru_mgr->disk_reads;
        result.disk_writes = g_lru_mgr->disk_writes;
        result.hits = g_lru_mgr->hits;
        result.misses = g_lru_mgr->misses;
        result.evictions = g_lru_mgr->evictions;
    } else if (strategy == "MRU" && g_mru_mgr) {
        for (size_t i = 0; i < trace.size(); ++i) {
            StepInfo s;
            Page* p = g_mru_mgr->access_page(trace[i], is_write, &s);
            if (!p) continue;
            StepLog log{static_cast<int>(i + 1), trace[i], s.hit, s.evicted_page, s.dirty_eviction, s.eviction_reason,
                        s.buffer_state, s.disk_reads, s.disk_writes};
            steps.push_back(log);
            g_mru_mgr->unpin_page(p);
        }
        result.accesses = g_mru_mgr->accesses;
        result.disk_reads = g_mru_mgr->disk_reads;
        result.disk_writes = g_mru_mgr->disk_writes;
        result.hits = g_mru_mgr->hits;
        result.misses = g_mru_mgr->misses;
        result.evictions = g_mru_mgr->evictions;
    } else if (strategy == "CLOCK" && g_clock_mgr) {
        for (size_t i = 0; i < trace.size(); ++i) {
            StepInfo s;
            Page* p = g_clock_mgr->access_page(trace[i], is_write, &s);
            if (!p) continue;
            StepLog log{static_cast<int>(i + 1), trace[i], s.hit, s.evicted_page, s.dirty_eviction, s.eviction_reason,
                        s.buffer_state, s.disk_reads, s.disk_writes};
            steps.push_back(log);
            g_clock_mgr->unpin_page(p);
        }
        result.accesses = g_clock_mgr->accesses;
        result.disk_reads = g_clock_mgr->disk_reads;
        result.disk_writes = g_clock_mgr->disk_writes;
        result.hits = g_clock_mgr->hits;
        result.misses = g_clock_mgr->misses;
        result.evictions = g_clock_mgr->evictions;
    }
}

}  // namespace

void set_server_results(const std::vector<SimResult>& results) {
    std::lock_guard<std::mutex> lock(g_results_mutex);
    g_results = results;
}

void run_http_server(int port) {
    httplib::Server server;

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream in("index.html");
        if (!in.is_open()) {
            res.status = 404;
            res.set_content("index.html not found", "text/plain");
            add_cors(res);
            return;
        }
        std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        res.set_content(html, "text/html");
        add_cors(res);
    });

    server.Get("/api/strategies", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[\"LRU\",\"MRU\",\"CLOCK\"]", "application/json");
        add_cors(res);
    });

    server.Get("/api/workloads", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[\"SELECT\",\"JOIN\"]", "application/json");
        add_cors(res);
    });

    server.Post("/api/run", [](const httplib::Request& req, httplib::Response& res) {
        const std::string strategy = extract_json_string(req.body, "strategy");
        const std::string selected_workload = extract_json_string(req.body, "workload");
        const std::string sql_query = extract_json_string(req.body, "sql_query");
        const int buffer_size = extract_json_int(req.body, "buffer_size", 8);
        std::string query_type;

        if ((strategy != "LRU" && strategy != "MRU" && strategy != "CLOCK") || buffer_size <= 0 ||
            !is_select_or_join_query(sql_query, query_type)) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid input. Use strategy LRU/MRU/CLOCK and SQL starting with SELECT (JOIN allowed).\"}",
                            "application/json");
            add_cors(res);
            return;
        }
        if ((selected_workload == "JOIN" && query_type != "JOIN") ||
            (selected_workload == "SELECT" && query_type != "SELECT")) {
            res.status = 400;
            res.set_content("{\"error\":\"Selected query type and SQL do not match.\"}", "application/json");
            add_cors(res);
            return;
        }

        QueryExecutionResult query_out = execute_sql_query("simulation.db", sql_query);
        if (!query_out.error.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"SQL execution failed\"}", "application/json");
            add_cors(res);
            return;
        }
        if (query_type == "JOIN") {
            write_join_output(query_out);
        }

        const std::string mapped_workload = map_query_to_workload(query_type);
        const std::vector<int> fallback_trace = get_page_trace("simulation.db", mapped_workload);
        const std::vector<int> trace = build_trace_from_query_result(query_type, query_out, fallback_trace);

        SimResult out;
        out.strategy = strategy;
        out.workload = query_type;
        out.buffer_size = buffer_size;

        std::vector<StepLog> steps;
        const bool is_write = false;
        run_trace_for_manager(strategy, buffer_size, trace, is_write, out, steps);

        out.total_io = out.disk_reads + out.disk_writes;
        const int total = out.hits + out.misses;
        out.hit_rate = total == 0 ? 0.0f : 100.0f * static_cast<float>(out.hits) / total;

        std::cout << "\n=== User Simulation Request ===\n";
        std::cout << "Query: " << query_type << " | Strategy: " << strategy << " | Buffer Size: " << buffer_size
                  << "\n";
        std::cout << "SQL: " << sql_query << "\n";
        std::cout << "Trace length: " << trace.size() << "\n";
        for (const StepLog& s : steps) {
            std::cout << "Step " << s.step << ": " << (s.hit ? "HIT" : "MISS") << " page " << s.page
                      << " | buffer=" << format_state(s.buffer_state)
                      << " | evicted=" << (s.evicted_page == -1 ? std::string("-") : std::to_string(s.evicted_page))
                      << " | reason=" << (s.eviction_reason.empty() ? "-" : s.eviction_reason)
                      << " | dirty_eviction=" << (s.dirty_eviction ? 1 : 0)
                      << " | reads=" << s.disk_reads << " writes=" << s.disk_writes << "\n";
        }
        std::cout << "Final: accesses=" << out.accesses << ", reads=" << out.disk_reads << ", writes="
                  << out.disk_writes << ", hits=" << out.hits << ", misses=" << out.misses << ", hit_rate="
                  << std::fixed << std::setprecision(2) << out.hit_rate << "%, evictions=" << out.evictions << "\n";

        {
            std::lock_guard<std::mutex> lock(g_results_mutex);
            g_results.push_back(out);
        }
        append_result_csv(out);

        res.set_content(run_json(out, steps, query_out), "application/json");
        add_cors(res);
    });

    server.Get("/api/results/all", [](const httplib::Request&, httplib::Response& res) {
        std::vector<SimResult> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_results_mutex);
            snapshot = g_results;
        }
        res.set_content(results_json_array(snapshot), "application/json");
        add_cors(res);
    });

    server.Get("/api/results/csv", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream in("results.csv");
        if (!in.is_open()) {
            res.status = 404;
            res.set_content("results.csv not found", "text/plain");
            add_cors(res);
            return;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        res.set_content(content, "text/csv");
        add_cors(res);
    });

    server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    std::cout << "Serving frontend and API on http://localhost:" << port << "\n";
    server.listen("0.0.0.0", port);
}
