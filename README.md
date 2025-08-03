
# Redis-Like In-Memory Database Server

A lightweight in-memory key-value store built from scratch in C++. Inspired by Redis, It uses a custom binary protocol, epoll-based non blocking I/O and, AOF (Append only file) persistence, also involves a hashtable implementation.



## Features
- `epoll` I/O: Handles concurrent client connections using non-blocking sockets.
- Custom key-value store: Supports key - value pairs using strings and hashtables.
- Persistence: Database is stored and restored using an AOF file during execution.
- Client binary: Communicates with the server using a simple command-line interface.


## Installation
### Dependencies
- Linux (epoll, sockets)
- g++ (C++11 or higher)
### Compile Server and Client

```bash
g++ -std=c++17 -Wall -o2 server server.cpp
g++ -std=c++17 -Wall -o2 client.cpp -o client
```
### Run Server
```
./server
```
### Run Client (In a separate terminal)
```
./client set foo bar
./client get foo
```

### Simulate concurrent Clients
We use a simple shell script to spawn multiple background client processes
```
chmod +x test_100_clients.sh
./test_100_clients.sh
```

## Supported Commands

| Command	    |  Description....|
|---------------|-----------------| 
|set key value  |Set string value for a Key|
|get key	    |Get string value for a Key|
|del key      |Delete string value based on a Key|
|keys         |Returns all Keys present in the Database|
|clear        |Clears out All Keys|
##AOF Format
- All write commands(set, del, clear) are logged in a binary AOF file
- File is replayed on server startup to restore data
- Automatically compacts every 1000 commands

## License
This project is licensed under the [MIT License](LICENSE).

## Acknowledgements
Inspired by Redis, this project aims to deepen understanding of Key-Value Databases, socket programming and concurrency in C++.
