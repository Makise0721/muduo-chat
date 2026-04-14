# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This repository contains a chat server system built on a custom C++ networking library (`mymuduo`). The project implements a Reactor-pattern TCP server supporting user registration/login, private messaging, group chats, friend management, and offline messaging. It uses MySQL for persistent storage and JSON over newline-delimited messages for client-server communication. Redis was originally included but has been removed as it's not essential for single-server operation.

## Building

The project uses CMake (3.10+) and requires C++11. Dependencies: MySQL client library, pthreads.

### Standard Build
```bash
mkdir build && cd build
cmake ..
make
```

Outputs:
- `bin/ChatServer` - main chat server executable
- `bin/ChatClient` - simple test client
- `mymuduo/lib/libmymuduo.so` - networking library shared object

### Build Notes
- The top-level `CMakeLists.txt` compiles both `mymuduo` (as a shared library) and `chatserver` (executable) together.
- Include paths are set to `thirdparty/`, `mymuduo/`, and `chatserver/include/`.
- MySQL library is located via `pkg-config`.
- Debug symbols (`-g`) are enabled by default.
- Optionally, `mymuduo/autobuild.sh` can install the network library system-wide (requires sudo).

## Running

### Prerequisites
1. MySQL server (5.7+) with database `chat` created via `sql/chat.sql`.
2. Environment variable `DB_PASSWORD` set to MySQL root password (defaults to "123456" if not set).

### Start the Server
```bash
./bin/ChatServer <ip> <port>
# Example: ./bin/ChatServer 127.0.0.1 6000
```

The server binds to the given IP and port, initializes the MySQL connection pool, and enters the event loop.

### Testing with Client
A simple test client `ChatClient` is built but not fully implemented; you can test with `telnet` or `nc`:
```bash
nc 127.0.0.1 6000
{"msgid":4,"name":"test","password":"123456"}
{"msgid":1,"id":1,"password":"123456"}
```

All messages must be JSON objects ending with a newline (`\n`).

## Architecture

### Network Layer (`mymuduo/`)
A Reactor‑pattern networking library modeled after muduo. Key components:
- **EventLoop**: per‑thread event dispatcher using `epoll` (via `EPollPoller`).
- **TcpServer**: manages acceptor, connection map, and `EventLoopThreadPool`.
- **TcpConnection**: represents a single TCP connection with read/write buffers.
- **Channel**: wraps a file descriptor with interest events and callbacks.
- **Buffer**: application‑level input/output buffer.

The library follows "one loop per thread" design; `TcpServer` can be configured with a thread pool to handle connections on multiple I/O threads.

### Business Layer (`chatserver/`)
- **ChatServer**: thin wrapper around `TcpServer`; registers `onConnection`/`onMessage` callbacks and forwards messages to `ChatService`.
- **ChatService**: singleton that implements all business logic. Maintains:
  - `_msgHandlerMap`: mapping from `msgid` (integer) to handler function.
  - `_userConnMap`: mapping from user ID to `TcpConnectionPtr` (for online users).
- Handlers (`login`, `reg`, `oneChat`, `groupChat`, etc.) execute SQL queries, manage user state, and route messages.

### Data Layer
- **MySQL**: wrapper around `mysql.h` providing `connect`, `query`, `update`, and prepared‑statement support.
- **ConnectionPool**: singleton pool of MySQL connections (RAII‑managed via `MySQLConnectionGuard`).
- Redis was originally included for cross‑server messaging but has been removed as it's not essential for single‑server operation.

### Protocol
- Messages are JSON objects, one per line.
- Each message contains a `msgid` field (see `EnMsgType` in `ChatService.hpp`).
- Server responds with corresponding `*_ACK` messages or forwards chat messages to recipients.

### Threading & Concurrency
- Network I/O runs on `EventLoop` threads (one per thread in the pool).
- `ChatService` uses a mutex (`_connMutex`) to protect `_userConnMap`.
- Database connections are thread‑local via the connection pool.
- Recent optimizations: `groupChat` copies online connections under a single lock to reduce contention.

## Development Notes

### Code Style
- Header guards use `#pragma once`.
- `using namespace std;` is present in some headers (legacy).
- Smart pointers (`std::shared_ptr`, `std::unique_ptr`) are used for resource ownership.
- Callbacks are `std::function` objects bound with `std::bind`.

### Safety & Security
- SQL injection is mitigated via `mysql_real_escape_string` (see `escapeString` in `ChatService.cpp`).
- Prepared‑statement infrastructure exists (`MySQL::prepareStatement`) but is not yet widely used.
- Buffer overflow protection: `sprintf` replaced with `snprintf`.
- RAII guards (`MySQLConnectionGuard`, `MySQLResultGuard`, `PreparedStatementGuard`) ensure resources are released.

### Testing & Examples
- No unit‑test framework is set up.
- **Echo server example**: Located at `mymuduo/example/testserver.cc`. To compile and run:
  ```bash
  cd mymuduo
  g++ -std=c++11 -I. example/testserver.cc *.cc -pthread -o testserver
  ./testserver
  ```
  The server listens on port 8000 and echoes back received data.
- **Chat server testing**: Use `telnet` or `nc` with JSON messages (see "Running" section).
- The `ChatClient` binary is minimal and not a full client.

### Common Tasks
- Adding a new message type:
  1. Add `msgid` to `EnMsgType` in `ChatService.hpp`.
  2. Add handler method to `ChatService` class.
  3. Register the handler in `ChatService` constructor.
  4. Implement the handler in `ChatService.cpp`.
- Modifying database schema: update `sql/chat.sql` and adjust relevant queries in `ChatService.cpp`.

### Known Issues / Tech Debt
- Prepared statements are not yet used for core business SQL (only escape‑string protection).
- The `ChatClient` executable is minimal and not a real client.
- Error handling is basic; many functions return `void` and assume success.

### Environment
- Developed on Linux (WSL2); depends on `epoll`.
- MySQL credentials are passed via `DB_PASSWORD` environment variable (defaults to "123456").
- The server expects MySQL on `127.0.0.1:3306`.

## References
- `README.md` – project overview, message format examples.
- `improve.md` – detailed logs of security and performance improvements.
- `mymuduo/README.md` – networking library documentation.
- `sql/chat.sql` – database schema.
