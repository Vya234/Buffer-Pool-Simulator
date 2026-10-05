# Buffer Pool Simulator — DBMS Buffer Management

> A C++17-based simulation framework for studying database buffer management using LRU, MRU, and CLOCK page replacement strategies, with SQLite integration and an interactive web-based visualization dashboard.

---

## 📌 Project Overview

**Buffer Pool Simulator** is a Database Management Systems project that simulates how a database buffer manager manages pages between disk and main memory.

The project implements and compares three classical buffer replacement strategies:

- **LRU — Least Recently Used**
- **MRU — Most Recently Used**
- **CLOCK — Second-Chance Replacement**

The simulator generates page-access traces from a SQLite database, replays them through each buffer manager, and evaluates their performance using disk I/O, hit rate, misses, and eviction statistics.

A browser-based dashboard allows users to configure simulations, execute SQL queries, visualize buffer states step-by-step, and compare the behaviour of different replacement strategies.

---

## 🎯 Objectives

- Implement LRU, MRU, and CLOCK buffer replacement strategies.
- Simulate database page accesses using SQLite workloads.
- Model buffer hits, misses, page evictions, dirty pages, and pinned pages.
- Measure disk reads, disk writes, total I/O, hit rate, and evictions.
- Study the effect of buffer pool size on performance.
- Provide an interactive visualization of buffer states.
- Compare different buffer replacement strategies under different workloads.

---

## 🧠 Buffer Replacement Strategies

### LRU — Least Recently Used

LRU evicts the page that has been unused for the longest period of time.

The implementation maintains pages in access order using a doubly-linked list. On every access, the page is moved to the most-recently-used end. When the buffer is full, the least recently used unpinned page is selected as the victim.

**Best suited for:** workloads with strong temporal locality.

---

### MRU — Most Recently Used

MRU evicts the page that was accessed most recently.

Although counter-intuitive for general workloads, MRU can perform well for repeated sequential scans where recently accessed pages are less likely to be reused immediately.

**Best suited for:** certain sequential-scan workloads.

---

### CLOCK — Second-Chance

CLOCK approximates LRU using a circular buffer and a reference bit.

Each buffer frame maintains a reference bit:

```text
Reference Bit = 1
       │
       ▼
Give page a second chance
       │
       ▼
Clear reference bit
       │
       ▼
Advance clock hand
```

If the clock hand encounters a frame whose reference bit is already 0, that frame can be selected for eviction.

**Advantages:**
- Lower overhead than strict LRU.
- Uses a simple circular buffer.
- Avoids continuous linked-list manipulation.
- Provides an efficient approximation of LRU.

---

## 🗄️ SQLite Integration

The simulator initializes a SQLite database at runtime and creates two tables:

### Students
| Column | Description |
| :--- | :--- |
| id | Student ID |
| name | Student name |
| age | Student age |
| dept | Department |

### Courses
| Column | Description |
| :--- | :--- |
| id | Course ID |
| student_id | Associated student |
| course | Course name |
| grade | Student grade |

The database is populated programmatically and is used to generate page-access traces for different workloads.

---

## Workloads

- **SELECT Scan:** Sequentially scans records from the Students table and generates a corresponding page-access trace.
- **SELECT Point:** Performs repeated point lookups against the Students table.
- **JOIN:** Simulates a join between the Students and Courses tables:
  ```sql
  SELECT s.id, c.id
  FROM Students s
  JOIN Courses c
  ON s.id = c.student_id;
  ```
  The resulting access pattern is converted into page requests and replayed through the buffer managers.
- **RANGE:** Generates page traces from range-based queries over student records.

---

## Performance Metrics

Each simulation records:
- **Disk Reads:** Number of pages loaded from disk
- **Disk Writes:** Number of dirty pages written back to disk
- **Total I/O:** Disk reads + disk writes
- **Hits:** Requests served directly from the buffer
- **Misses:** Requests requiring a page load
- **Hit Rate:** Percentage of requests served from the buffer
- **Evictions:** Number of pages removed from the buffer

$$\text{Hit Rate} = \left(\frac{\text{Hits}}{\text{Total Accesses}}\right) \times 100$$

---

## Interactive Web Dashboard

The project includes an embedded HTTP server and browser-based dashboard. After starting the simulator, open:

```text
http://localhost:8080
```

### Features
- Select LRU / MRU / CLOCK strategies
- Select workload type
- Configure buffer size
- Enter SELECT/JOIN SQL queries
- Run simulations
- Replay buffer accesses step-by-step
- View buffer state after every access
- View hits and misses
- Track disk reads and writes
- View page evictions
- Compare disk I/O across buffer sizes
- Compare hit rates
- View SQL query results
- Download simulation results as CSV

---

## 🏗️ System Architecture

```text
                     ┌─────────────────────┐
                     │    SQLite Database  │
                     │ Students / Courses  │
                     └──────────┬──────────┘
                                │
                                ▼
                     ┌─────────────────────┐
                     │ Workload & Page     │
                     │ Trace Generation    │
                     └──────────┬──────────┘
                                │
                                ▼
              ┌─────────────────────────────────┐
              │       Buffer Pool Simulator     │
              │                                 │
              │   ┌─────┐ ┌─────┐ ┌─────────┐   │
              │   │ LRU │ │ MRU │ │  CLOCK  │   │
              │   └─────┘ └─────┘ └─────────┘   │
              └────────────────┬────────────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ Performance Metrics │
                    │                     │
                    │ Reads / Writes      │
                    │ Hits / Misses       │
                    │ Hit Rate            │
                    │ Evictions           │
                    │ Total I/O           │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  Web Dashboard      │
                    │  Visualization      │
                    └─────────────────────┘
```

---

## 🛠️ Tech Stack

| Component | Technology |
| :--- | :--- |
| **Core Implementation** | C++17 |
| **Database** | SQLite3 |
| **HTTP Server** | cpp-httplib |
| **Frontend** | HTML, CSS, JavaScript |
| **Visualization** | Chart.js |
| **Build System** | Make |

---

## 📁 Project Structure

```text
buffer-pool-simulator/
│
├── main.cpp
├── defs.h
├── defs.cpp
├── sqlite_integration.h
├── sqlite_integration.cpp
├── server.cpp
├── httplib.h
├── index.html
├── Makefile
├── README.md
└── .gitignore
```

### File Description

| File | Description |
| :--- | :--- |
| `main.cpp` | Initializes the database and starts the HTTP server |
| `defs.h` | Declarations for Page, LRU, MRU and CLOCK buffer managers |
| `defs.cpp` | Implementation of LRU, MRU and CLOCK |
| `sqlite_integration.h` | SQLite helper declarations |
| `sqlite_integration.cpp` | SQLite initialization and page-trace generations |
| `server.cpp` | HTTP server, simulation logic and API routes |
| `httplib.h` | Bundled cpp-httplib library |
| `index.html` | Interactive browser dashboard |
| `Makefile` | Build configuration |

---

## ⚙️ Requirements

- **OS:** Ubuntu / Debian
- **Compiler:** GCC / G++ (C++17 or later)
- **Library:** SQLite3 development library
- **Build Tool:** Make

### Install Dependencies:

```bash
sudo apt update
sudo apt install g++ libsqlite3-dev make
```

---

## Getting Started

### 1. Clone the Repository
```bash
git clone https://github.com/Vya234/Buffer-Pool-Simulator.git
cd Buffer-Pool-Simulator
```

### 2. Build
```bash
make
```

Or compile manually:
```bash
g++ -std=c++17 -O2 \
    -o buffer_sim \
    main.cpp \
    defs.cpp \
    sqlite_integration.cpp \
    server.cpp \
    -lsqlite3
```

### 3. Run
```bash
./buffer_sim
```

Expected output:
```text
Database initialized at simulation.db
Open http://localhost:8080 in your browser
Press Ctrl+C to stop server.
```

### 4. Open the Dashboard
Open the following URL in your browser:
```text
http://localhost:8080
```

---

## Implementation Details

### Page Representation
Each page stores metadata including:
- File pointer
- Page number
- Dirty flag
- Pin status
- Reference bit
- Validity
- Frame index
- Last-access timestamp

The simulator uses a fixed page size of **512 bytes**.

### Page Lookup
Pages are indexed using a hash map keyed by `(FILE*, page_number)`, providing average $O(1)$ lookup.

### Pinning
Pinned pages **cannot** be selected as eviction victims.

### Dirty Pages
When a dirty page is selected for eviction:
$$\text{Dirty Page} \longrightarrow \text{Write Page to Disk} \longrightarrow \text{Evict Page} \longrightarrow \text{Load New Page}$$

### CLOCK Reference Bit
CLOCK uses a reference bit for each frame:
$$\text{ref\_bit} = 1 \longrightarrow \text{Second chance} \longrightarrow \text{ref\_bit} = 0 \longrightarrow \text{Next clock sweep can evict}$$

---

## Simulation Flow

```text
                    Buffer Size
                         │
                         ▼
                 Select Strategy
                         │
              ┌──────────┼──────────┐
              ▼          ▼          ▼
             LRU        MRU       CLOCK
              │          │          │
              └──────────┼──────────┘
                         ▼
                  Select Workload
                         │
                   ┌─────┴─────┐
                   ▼           ▼
                SELECT        JOIN
                   │           │
                   └─────┬─────┘
                         ▼
                Generate Page Trace
                         │
                         ▼
                  Replay Page Trace
                         │
                         ▼
                  Buffer Manager
                         │
                         ▼
                  Calculate Metrics
                         │
                         ▼
             ┌───────────────────────┐
             │ Reads / Writes        │
             │ Hits / Misses         │
             │ Hit Rate              │
             │ Evictions             │
             │ Total I/O             │
             └───────────────────────┘
```

---

## Generated Files

The following files are generated while running the simulator:
- `simulation.db` — SQLite database
- `results.csv` — Simulation metrics
- `log_clock.txt` — CLOCK access log
- `log_lru.txt` — LRU access log
- `log_mru.txt` — MRU access log
- `join_output.txt` — JOIN query output
- `data.txt` — Database data dump
- `buffer_sim` — Compiled executable

---

## Learning Outcomes

This project provides practical understanding of:
- Database buffer management
- Page replacement algorithms
- Cache management
- Disk I/O optimization
- SQLite integration
- Hash-based page lookup
- Dirty-page management
- Page pinning
- HTTP server integration
- REST API design
- Browser-based visualization
- Performance benchmarking

---

## Future Improvements

- Add FIFO replacement policy
- Add LFU replacement policy
- Configurable page size
- Configurable database size
- Larger synthetic workloads
- Real database-file page mapping
- I/O latency simulation
- Automated benchmark generation
- Exportable performance graphs
- Multi-threaded workload simulation
- Persistent simulation history
- Enhanced dashboard animations