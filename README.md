# Kayles UDP Network Game

![C](https://img.shields.io/badge/c-gnu11-blue.svg)
![Protocol](https://img.shields.io/badge/protocol-UDP%20%7C%20IPv4-lightgrey)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20POSIX-brightgreen)

A high-performance, client-server implementation of a modified Kayles board game over a custom UDP binary application protocol. Written in standard GNU11 using POSIX sockets.

## Architecture & Network Protocol

This project demonstrates advanced systems programming and network engineering concepts:
* **Custom Binary Protocol:** Communication is handled via raw UDP datagrams. The protocol uses strict `__attribute__((__packed__))` structs to prevent compiler padding and memory alignment issues during serialization.
* **Network Byte Order (Endianness):** All multi-byte integers (Player IDs, Game IDs) are strictly converted to Big-Endian (`htonl`/`ntohl`) before transmission to ensure cross-platform compatibility.
* **Bitmapped Game State:** The game board is transmitted using a highly compressed bit-mapped array. A Flexible Array Member (FAM) is used in the struct to support dynamic board sizes while maintaining a flat memory layout. Bitwise operations handle game logic (O(1) updates).
* **High-Performance Server:** The server operates as a single-threaded state machine. It uses an O(1) pre-allocated object pool for game sessions and implements a custom Garbage Collector to clean up expired sessions and handle disconnected clients.

## Build Instructions

Requires a GNU11 compliant compiler and GNU Make.

```bash
git clone https://github.com/Pavelpuff/c-udp-kayles-game.git
cd c-udp-kayles-game
make
```
This will compile two executables: `kayles_server` and `kayles_client`.

## Usage

### Starting the Server
The server requires an initial board layout (`-r`, where '1' is a pawn and '0' is empty space), a listening IP (`-a`), a port (`-p`), and a timeout in seconds (`-t`).

```bash
# Listen on all interfaces (0.0.0.0), port 8080, 30s timeout, board: 11110111
./kayles_server -r 11110111 -a 0.0.0.0 -p 8080 -t 30
```

### Starting the Client
Players interact with the server by sending specific binary payload parameters encoded as a string with the `-m` flag (Format: `MSG_TYPE/PLAYER_ID/GAME_ID/PAWN_INDEX`).

```bash
# Player 42 joins the matchmaking queue (MSG_TYPE 0)
./kayles_client -a 127.0.0.1 -p 8080 -m "0/42" -t 5

# Player 42 knocks down two pawns starting at index 1 in Game 1 (MSG_TYPE 2)
./kayles_client -a 127.0.0.1 -p 8080 -m "2/42/1/1" -t 5
```

### Message Types
* `0`: Join Game (`MSG_JOIN`)
* `1`: Knock down 1 pawn (`MSG_MOVE_1`)
* `2`: Knock down 2 pawns (`MSG_MOVE_2`)
* `3`: Keep-Alive Ping (`MSG_KEEP_ALIVE`)
* `4`: Resign/Give Up (`MSG_GIVE_UP`)