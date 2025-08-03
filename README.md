
# Redis-Like In-Memory Database Server

A lightweight in-memory key-value store built from scratch in C++. Inspired by Redis, this project supports strings and persistence via a custom binary format, and concurrent clients using `epoll`.



## Features
- `epoll` I/O: Handles concurrent client connections using non-blocking sockets.
- Custom key-value store: Supports key - value pairs using strings and hashtables.
- Persistence: Database is saved to and restored from a custom binary file.
- Client binary: Communicates with the server using a simple command-line interface.


## Installation

### Compile Server and Client

```bash
g++ -g -o server server.cpp
g++ -o2 -g client.cpp -o client
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
|set key value  |Set string value |
|get key	    |Get string value |
|del key      |Delete string value based on Key|
|clear        |Clears out All keys|
## License
This project is licensed under the [MIT License](LICENSE).

## Acknowledgements
Inspired by Redis, this project aims to deepen understanding of Key-Value Databases, socket programming and concurrency in C++.
