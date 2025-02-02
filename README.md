# Battleship Multiplayer Game - Network Programming Project

## Authors
**Mirosław Baca, Marcel Gacoń**  
AGH University - Network Programming

## Project Overview
This project implements a **multiplayer Battleship game** over the network. The game server manages multiple games simultaneously, where two players compete, and additional users can join as **observers**. The client application connects to the server via **multicast discovery** and then establishes a **unicast TCP connection** for stable communication. The game logic, including **ship placement, shooting, and turn-based mechanics**, is handled between the clients and the server.

The server operates as a **daemon** and logs match results.

---

## How It Works
### Client-Server Architecture:
1. **Multicast Server Discovery**: Clients send a query to a **multicast group**, and the server responds with its **IP and port**.
2. **TCP Connection**: Once discovered, the client connects via **unicast TCP**.
3. **Lobby System**: Players join a waiting area (**lobby**) where they can:
   - **/create** → Create a new game.
   - **/join <id>** → Join an existing game or become an observer.
   - **/list** → View available games.
   - **/exit** → Leave the game.

### Gameplay:
- **/place** → Players place their ships before the game starts.
- **/start** → Players confirm readiness.
- **/fire x y** → Attack a coordinate on the opponent’s board.
- The game **starts** when both players confirm readiness.
- Players can **chat** during the match.
- **Observers** can view real-time board updates but **cannot interact**.
- After a match, **players and observers return to the lobby**.

### Server Shutdown:
- The server properly **closes sockets** and **frees memory** when terminated (`Ctrl + C`).
- If running as a **daemon**, it cleans up resources upon receiving a **kill signal**.

---

## Features & Security Measures
- **Client-server architecture with multithreading** (each client runs in a separate thread).
- **Multicast-based server discovery** (clients find the server via multicast queries).
- **TCP Unicast Communication** (ensuring stable data transmission).
- **Binary TLV-based data transfer** for observers (**game board updates** are sent as TLV instead of text for efficiency).
- **Daemon mode** (server can run in the background without a terminal).
- **Match logging** to `battleship.log` (records game results).
- **Error handling for network functions** (`recv`, `send`, `socket`, `bind`).
- **Resource cleanup** (closing sockets, freeing memory).
- **Signal handling** (`Ctrl+C` safely shuts down the server).
- **Scalability** (adjustable game board sizes and player limits).
- **Username validation** (prevents duplicate usernames).
- **Client timeout during username handshake** (disconnects inactive users).
- **Address conversion using `inet_pton`** (handles IP addresses correctly).
- **Blocking `/place` command in the lobby** (prevents ship placement before entering a game).
- **Fire and hit tracking** (separate boards for shots fired and hit markers).
- **Direct server connection option** (`--serverIP <address>` for manual connection).
- **Graceful `/exit` handling** (removes the user from the game and frees resources).
- **Automatic return to the lobby** after a match.

---

## Compilation & Execution

### Compile:
```sh
gcc -o client klient.c -pthread
gcc -o server serwer.c -pthread
