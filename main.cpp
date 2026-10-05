#include "sqlite_integration.h"

#include <iostream>
#include <string>

void run_http_server(int port);

int main() {
    const std::string db_path = "simulation.db";
    init_sqlite_db(db_path);
    std::cout << "Database initialized at simulation.db\n";
    std::cout << "Open http://localhost:8080 in your browser\n";
    std::cout << "Press Ctrl+C to stop server.\n";
    run_http_server(8080);
    close_sqlite_db();
    return 0;
}
