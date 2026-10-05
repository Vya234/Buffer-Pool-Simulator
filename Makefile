all:
	g++ -std=c++17 -O2 -o buffer_sim main.cpp defs.cpp sqlite_integration.cpp server.cpp -lsqlite3
run:
	./buffer_sim
