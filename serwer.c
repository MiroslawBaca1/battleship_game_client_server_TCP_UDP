/*
 * Copyright (c) 2025 Miroslaw Baca & Marcel Gacoń
 * AGH - Programowanie sieciowe
 */

// ==================== Includy i Definicje ====================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h> // Demon

#define MAX_CLIENTS     10
#define SERVER_PORT     12345
#define DISCOVERY_PORT  12346
#define MULTICAST_ADDR  "239.255.0.1"
#define USERNAME_HANDSHAKE_TIMEOUT 5

// Definicja trybu demona. 1 aby uruchomić serwer jako demona, 0 aby uruchomić normalnie.
#define RUN_AS_DAEMON 0

#define WELCOME_IN_LOBBY \
"ENTERING_LOBBY\n" \
"[INFO] Lobby/Chat commands:\n" \
"  /create           - create a new room\n" \
"  /join <id>        - join a room\n" \
"  /list             - list rooms\n" \
"  /exit             - leave room or quit\n\n" \
"[BATTLESHIP] Additional commands:\n" \
"  /place            - place your ships locally (only once)\n" \
"  /start            - confirm readiness (only after placing ships)\n" \
"  /fire x y         - shoot at (x,y) if it's your turn\n\n"

#define BUFFER_SIZE    1024   // Rozmiar bufora wiadomości
static char msg[BUFFER_SIZE];     // Bufor do tworzenia komunikatów

// ==================== Struktury Danych ====================

typedef struct {
    int socket;
    struct sockaddr_in address;
    char username[50];
    int room_id;
    int active;
    int tlv_socket; // Gniazdo dla połączenia TLV, jeśli dotyczy
} Client;

typedef struct {
    int id;
    char creator[50];
    Client *clients[2];
    Client *observers[MAX_CLIENTS-2];
    int observer_count;
    int playerReady[2];
    int gameStarted;
    int current_turn;
    char boardPlayer0[64];
    char boardPlayer1[64];
} ChatRoom;

// ==================== Zmienne Globalne i Mutexy ====================

int server_fd;
int udp_sock;

// Zmienne do obsługi połączeń TLV
static int tlv_server_fd;
static int tlv_port;
pthread_t tlv_thread;

static ChatRoom chat_rooms[MAX_CLIENTS];
static int room_count = 0;

static Client *clients[MAX_CLIENTS];
static int client_count = 0;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rooms_mutex   = PTHREAD_MUTEX_INITIALIZER;

// ==================== Obsługa Sygnałów ====================
// Funkcja obsługująca sygnał SIGINT. Zamyka wszystkie gniazda i kończy działanie serwera.
void handle_sigint(int sig) {
    printf("Shutting down server...\n");
    close(server_fd);      // Zamknięcie gniazda TCP
    close(udp_sock);       // Zamknięcie gniazda UDP
    close(tlv_server_fd);  // Zamknięcie gniazda TLV
    exit(0);
}

// ==================== Uruchamianie Demona ====================
#if RUN_AS_DAEMON
void daemonize() {
    pid_t pid;
    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);  // Proces rodzicielski kończy działanie
    }
    if (setsid() < 0) {
        perror("setsid failed");
        exit(EXIT_FAILURE);
    }
    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);  // Drugi proces rodzicielski kończy działanie
    }
    chdir("/");
    umask(0);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}
#endif


// ==================== Funkcje Pomocnicze ====================

// Loguje wynik gry do pliku "battleship.log"
void log_game_result(const char *winner, const char *loser) {
    FILE *log_file = fopen("battleship.log", "a");
    if (!log_file) {
        perror("[SERVER] Failed to open log file");
        return;
    }
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);
    fprintf(log_file, "%s Player %s won with player %s\n", time_buf, winner, loser);
    fclose(log_file);
}

// Wysyła wiadomość do klienta
void send_to_client(int client_socket, const char *message) {
    if (client_socket <= 0 || message == NULL)
        return;
    send(client_socket, message, strlen(message), 0);
}

// Rozsyła wiadomość do wszystkich uczestników pokoju, z opcjonalnym wykluczeniem jednego gniazda
void broadcast_to_room(ChatRoom *room, const char *message, int exclude_socket) {
    if (!room)
        return;
    for (int i = 0; i < 2; i++) {
        if (room->clients[i] && room->clients[i]->active) {
            if (room->clients[i]->socket != exclude_socket) {
                send_to_client(room->clients[i]->socket, message);
            }
        }
    }
    for (int i = 0; i < room->observer_count; i++) {
        if (room->observers[i] && room->observers[i]->active) {
            if (room->observers[i]->socket != exclude_socket) {
                send_to_client(room->observers[i]->socket, message);
            }
        }
    }
}

// Zwraca wskaźnik do pokoju o podanym ID
ChatRoom* get_room_by_id(int room_id) {
    if (room_id < 0 || room_id >= room_count)
        return NULL;
    return &chat_rooms[room_id];
}

// Informuje obserwatora o stanie gry
void notify_observer_about_game_state(ChatRoom *room, int observer_socket) {
    if (!room->gameStarted)
        send_to_client(observer_socket, "GAME_NOT_STARTED\n");
    else
        send_to_client(observer_socket, "GAME_STARTED\n");
}

// Rozpoczyna grę w danym pokoju - ustawia flagi i wysyła komunikaty do graczy
void start_game(ChatRoom *room) {
    room->gameStarted = 1;
    broadcast_to_room(room, "GAME_START\n", -1);
    room->current_turn = 0;
    if (room->clients[0]) {
        char buf[BUFFER_SIZE];
        snprintf(buf, sizeof(buf), "NEXT_TURN %s\n", room->clients[0]->username);
        broadcast_to_room(room, buf, -1);
    }
}

// ==================== Wątek UDP Discovery ====================
// Wątek obsługujący wykrywanie serwera przez UDP multicast
void* udp_discovery_thread(void* arg) {
    char *interface_name = (char *)arg;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len;
    char buffer[BUFFER_SIZE];

    if ((udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket UDP");
        pthread_exit(NULL);
    }

    struct ip_mreq mreq;
    struct in_addr local_interface;
    if (inet_aton(interface_name, &local_interface) == 0) {
        perror("Invalid interface IP");
        close(udp_sock);
        pthread_exit(NULL);
    }
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_ADDR);
    mreq.imr_interface = local_interface;

    if (setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (char *)&mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        close(udp_sock);
        pthread_exit(NULL);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port        = htons(DISCOVERY_PORT);

    if (bind(udp_sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind UDP");
        close(udp_sock);
        pthread_exit(NULL);
    }

    int ttl = 5;
    setsockopt(udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    printf("UDP discovery thread: listening on port %d, joined group %s\n", DISCOVERY_PORT, MULTICAST_ADDR);
    const char *server_ip_for_discovery = interface_name;

    while (1) {
        len = sizeof(cliaddr);
        int n = recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&cliaddr, &len);
        if (n < 0) {
            perror("recvfrom UDP");
            continue;
        }
        buffer[n] = '\0';
        if (strstr(buffer, "DISCOVERY_REQUEST")) {
            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response),
                     "SERVER_IP=%s:%d", server_ip_for_discovery, SERVER_PORT);
            sendto(udp_sock, response, strlen(response), 0, (struct sockaddr*)&cliaddr, len);
        }
    }

    close(udp_sock);
    pthread_exit(NULL);
}

// ==================== Obsługa Użytkowników ====================

// Sprawdza, czy dany username jest już zajęty przez kogoś aktywnego
int is_username_taken(const char *uname) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i] && clients[i]->active) {
            if (strcmp(clients[i]->username, uname) == 0)
                return 1;
        }
    }
    return 0;
}

// Negocjuje z nowym klientem jego nazwę użytkownika i w pętli pyta, dopóki nazwa nie będzie wolna.
int negotiate_username(Client *client) {
    printf("[SERVER] Sending: Enter your username:\n");
    send_to_client(client->socket, "Enter your username:\n");

    struct timeval tv;
    tv.tv_sec = USERNAME_HANDSHAKE_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (1) {
        char buf[50];
        int n = recv(client->socket, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if(n == 0)
                printf("[SERVER] Client disconnected during handshake.\n");
            else
                perror("[SERVER] Timeout or error while waiting for username");
            send_to_client(client->socket, "You were disconnected due to inactivity.\n");
            close(client->socket);
            return -1;
        }
        buf[n] = '\0';
        char *p = strchr(buf, '\n');
        if (p)
            *p = '\0';
        p = strchr(buf, '\r');
        if (p)
            *p = '\0';
        if (strlen(buf) == 0) {
            send_to_client(client->socket, "Username cannot be empty, try again.\nEnter your username:\n");
            continue;
        }
        pthread_mutex_lock(&clients_mutex);
        int taken = is_username_taken(buf);
        pthread_mutex_unlock(&clients_mutex);
        if (taken) {
            send_to_client(client->socket, "Username in use, try again.\nEnter your username:\n");
        } else {
            strncpy(client->username, buf, sizeof(client->username)-1);
            client->username[sizeof(client->username)-1] = '\0';
            send_to_client(client->socket, "Username accepted\n");
            tv.tv_sec = 0;
            setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            return 0;
        }
    }
}

// ==================== Obsługa TLV ====================

static void send_board_update_to_observers(ChatRoom *room) {
    for (int i = 0; i < room->observer_count; i++) {
        Client *obs = room->observers[i];
        if (!obs->active)
            continue;
        int sock = obs->tlv_socket;
        if (sock <= 0)
            continue;
        unsigned char packet[3+64];
        // Wysyłamy planszę gracza 0
        packet[0] = 0x01; // Typ
        packet[1] = 0x00; // Długość (high byte)
        packet[2] = 64;   // Długość (low byte)
        memcpy(packet+3, room->boardPlayer0, 64);
        if (send(sock, packet, 67, 0) < 0)
            perror("[TLV] Failed to send boardPlayer0");
        else
        {
            //printf("[DEBUG][TLV] Sent boardPlayer0 update to observer %s\n", obs->username);
        }

        // Wysyłamy planszę gracza 1
        packet[0] = 0x02; // Typ
        packet[1] = 0x00;
        packet[2] = 64;
        memcpy(packet+3, room->boardPlayer1, 64);
        if (send(sock, packet, 67, 0) < 0)
            perror("[TLV] Failed to send boardPlayer1");
        else
        {
            // Pomyślnie przesłano
        }
    }
}

// Wątek akceptujący połączenia TLV od obserwatorów
static void *tlv_accept_thread(void *arg) {
    (void)arg; // Argument nie jest używany
    while (1) {
        struct sockaddr_in obs_addr;
        socklen_t obs_len = sizeof(obs_addr);
        int obs_sock = accept(tlv_server_fd, (struct sockaddr*)&obs_addr, &obs_len);
        if (obs_sock < 0) {
            perror("[TLV] Accept failed");
            continue;
        }
        printf("[TLV] Accepted new observer connection.\n");

        // Odbieramy nazwę użytkownika wysłaną przez obserwatora
        char tlv_username[50];
        int n = recv(obs_sock, tlv_username, sizeof(tlv_username) - 1, 0);
        if (n > 0) {
            tlv_username[n] = '\0';
            printf("[TLV] Received TLV username: %s\n", tlv_username);
            // Mapujemy gniazdo TLV do odpowiedniego klienta
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < client_count; i++) {
                if (clients[i] && clients[i]->active &&
                    strcmp(clients[i]->username, tlv_username) == 0) {
                    clients[i]->tlv_socket = obs_sock;
                    printf("[TLV] Mapped TLV socket to observer %s\n", tlv_username);
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        } else {
            printf("[TLV] Failed to receive TLV username, closing connection.\n");
            close(obs_sock);
        }
    }
    return NULL;
}

// ==================== Obsługa Klienta ====================

// Wątek obsługujący komunikację z pojedynczym klientem
void *handle_client(void *arg) {
    Client *client = (Client *)arg;
    char buffer[BUFFER_SIZE];

    // Po udanym handshake wysyłamy komunikat lobby
    send_to_client(client->socket, WELCOME_IN_LOBBY);

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_received = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0)
            break;
        buffer[bytes_received] = '\0';

        printf("[SERVER DEBUG]: %s: %s\n", client->username, buffer);

        // Obsługa aktualizacji planszy dla TLV
        pthread_mutex_lock(&rooms_mutex);
        if (strncmp(buffer, "BOARD0 ", 7) == 0 && client->room_id != -1) {
            ChatRoom *r = get_room_by_id(client->room_id);
            if (r) {
                const char *dat = buffer + 7;
                if (strlen(dat) >= 64) {
                    memcpy(r->boardPlayer0, dat, 64);
                    send_board_update_to_observers(r);
                }
            }
            pthread_mutex_unlock(&rooms_mutex);
            continue;
        }
        if (strncmp(buffer, "BOARD1 ", 7) == 0 && client->room_id != -1) {
            ChatRoom *r = get_room_by_id(client->room_id);
            if (r) {
                const char *dat = buffer + 7;
                if (strlen(dat) >= 64) {
                    memcpy(r->boardPlayer1, dat, 64);
                    send_board_update_to_observers(r);
                }
            }
            pthread_mutex_unlock(&rooms_mutex);
            continue;
        }
        pthread_mutex_unlock(&rooms_mutex);

        if (buffer[0] != '/') {
            if ((strncmp(buffer, "FIRE ", 5) != 0) &&
                (strncmp(buffer, "HIT ", 4) != 0) &&
                (strncmp(buffer, "MISS ", 5) != 0) &&
                (strncmp(buffer, "YOU_WIN", 7) != 0))
            {
                pthread_mutex_lock(&rooms_mutex);
                if (client->room_id != -1) {
                    ChatRoom *room = get_room_by_id(client->room_id);
                    if (!room) {
                        send_to_client(client->socket, "Error: room not found.\n");
                        client->room_id = -1;
                        pthread_mutex_unlock(&rooms_mutex);
                        continue;
                    }
                    int isPlayer = ((room->clients[0] == client) ||
                                     (room->clients[1] == client));
                    if (!isPlayer) {
                        send_to_client(client->socket, "Observer cannot send messages.\n");
                        pthread_mutex_unlock(&rooms_mutex);
                        continue;
                    }
                    snprintf(msg, sizeof(msg), "%s: %s\n", client->username, buffer);
                    broadcast_to_room(room, msg, -1);
                } else {
                    send_to_client(client->socket, "You are in the lobby. No chat here.\n");
                }
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
        }

        if (strncmp(buffer, "/exit", 5) == 0) {
            pthread_mutex_lock(&rooms_mutex);
            if (client->room_id != -1) {
                ChatRoom *room = get_room_by_id(client->room_id);
                if (room) {
                    if (room->clients[0] == client)
                        room->clients[0] = NULL;
                    else if (room->clients[1] == client)
                        room->clients[1] = NULL;
                    else {
                        for (int i = 0; i < room->observer_count; i++) {
                            if (room->observers[i] == client) {
                                for (int j = i; j < room->observer_count - 1; j++) {
                                    room->observers[j] = room->observers[j+1];
                                }
                                room->observer_count--;
                                break;
                            }
                        }
                    }
                }
                client->room_id = -1;
                send_to_client(client->socket, "You are now in the lobby.\n");
                if (room->clients[0])
                    send_to_client(room->clients[0]->socket, WELCOME_IN_LOBBY);
            } else {
                send_to_client(client->socket, "Goodbye.\n");
                pthread_mutex_unlock(&rooms_mutex);
                break;
            }
            pthread_mutex_unlock(&rooms_mutex);
            continue;
        }

        if (client->room_id == -1) {
            if (strncmp(buffer, "/create", 7) == 0) {
                pthread_mutex_lock(&rooms_mutex);
                ChatRoom *room = &chat_rooms[room_count];
                room->id = room_count;
                strncpy(room->creator, client->username, sizeof(room->creator)-1);
                room->creator[sizeof(room->creator)-1] = '\0';

                room->clients[0] = client;
                room->clients[1] = NULL;
                room->observer_count = 0;
                room->playerReady[0] = 0;
                room->playerReady[1] = 0;
                room->gameStarted = 0;
                room->current_turn = 0;
                memset(room->boardPlayer0, '.', 64);
                memset(room->boardPlayer1, '.', 64);

                client->room_id = room->id;
                room_count++;

                send_to_client(client->socket, "JOINED_ROOM\n");
                snprintf(msg, sizeof(msg),
                         "Room %d created by %s.\n"
                         "Wait for /join <id> from second player.\n",
                         room->id, room->creator);
                send_to_client(client->socket, msg);
                pthread_mutex_unlock(&rooms_mutex);
            }
            else if (strncmp(buffer, "/join ", 6) == 0) {
                int rid = atoi(buffer + 6);
                pthread_mutex_lock(&rooms_mutex);
                ChatRoom *room = get_room_by_id(rid);
                if (!room) {
                    send_to_client(client->socket, "Invalid room ID.\n");
                    pthread_mutex_unlock(&rooms_mutex);
                } else {
                    if (room->clients[0] && room->clients[1]) {
                        room->observers[room->observer_count++] = client;
                        client->room_id = rid;
                        send_to_client(client->socket, "JOINED_ROOM_OBSERVER\n");
                        send_to_client(client->socket, "Room is full. Joined as observer.\n");
                        snprintf(msg, sizeof(msg), "TLV_PORT %d\n", tlv_port);
                        send_to_client(client->socket, msg);
                        notify_observer_about_game_state(room, client->socket);
                        pthread_mutex_unlock(&rooms_mutex);
                    }
                    else if (room->clients[0] && !room->clients[1]) {
                        room->clients[1] = client;
                        client->room_id = rid;
                        send_to_client(client->socket, "JOINED_ROOM\n");
                        snprintf(msg, sizeof(msg),
                                 "Joined room %d as second player. Now 2 players in room.\n", rid);
                        send_to_client(client->socket, msg);
                        snprintf(msg, sizeof(msg),
                                 "%s joined as second player.\n", client->username);
                        broadcast_to_room(room, msg, client->socket);
                        pthread_mutex_unlock(&rooms_mutex);
                    }
                    else if (!room->clients[0]) {
                        room->clients[0] = client;
                        client->room_id = rid;
                        send_to_client(client->socket, "JOINED_ROOM\n");
                        send_to_client(client->socket, "Joined room as first player.\n");
                        memset(room->boardPlayer0, '.', 64);
                        memset(room->boardPlayer1, '.', 64);
                        snprintf(msg, sizeof(msg),
                                 "%s joined as first player.\n", client->username);
                        broadcast_to_room(room, msg, client->socket);
                        pthread_mutex_unlock(&rooms_mutex);
                    }
                    else {
                        send_to_client(client->socket, "Could not join.\n");
                        pthread_mutex_unlock(&rooms_mutex);
                    }
                }
            }
            else if (strncmp(buffer, "/list", 5) == 0) {
                pthread_mutex_lock(&rooms_mutex);
                if (room_count == 0) {
                    send_to_client(client->socket, "No rooms.\n");
                } else {
                    snprintf(msg, sizeof(msg), "Rooms: %d\n", room_count);
                    send_to_client(client->socket, msg);
                    for (int i = 0; i < room_count; i++) {
                        ChatRoom *r = &chat_rooms[i];
                        int countPlayers = 0;
                        if (r->clients[0])
                            countPlayers++;
                        if (r->clients[1])
                            countPlayers++;
                        snprintf(msg, sizeof(msg),
                                 "ID:%d by:%s players:%d/2\n",
                                 r->id, r->creator, countPlayers);
                        send_to_client(client->socket, msg);
                    }
                }
                pthread_mutex_unlock(&rooms_mutex);
            }
            else {
                send_to_client(client->socket, "Invalid command in lobby.\n");
            }
        }
        else {
            pthread_mutex_lock(&rooms_mutex);
            ChatRoom *room = get_room_by_id(client->room_id);
            if (!room) {
                send_to_client(client->socket, "Error: room not found.\n");
                client->room_id = -1;
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            int isPlayer = (room->clients[0] == client || room->clients[1] == client);
            int pIndex = -1;
            if (room->clients[0] == client)
                pIndex = 0;
            if (room->clients[1] == client)
                pIndex = 1;
            if (strncmp(buffer, "/start", 6) == 0) {
                if (!isPlayer) {
                    send_to_client(client->socket, "Observer cannot /start.\n");
                    pthread_mutex_unlock(&rooms_mutex);
                    continue;
                }
                room->playerReady[pIndex] = 1;
                char tmp[BUFFER_SIZE];
                snprintf(tmp, sizeof(tmp), "%s is ready.\n", client->username);
                broadcast_to_room(room, tmp, -1);
                if (room->clients[0] && room->clients[1]) {
                    if (!room->gameStarted) {
                        if (room->playerReady[0] && room->playerReady[1]) {
                            start_game(room);
                        }
                    }
                }
                else {
                    send_to_client(client->socket, "Waiting for second player...\n");
                }
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            else if (strncmp(buffer, "FIRE ", 5) == 0) {
                if (!isPlayer) {
                    send_to_client(client->socket, "Observer cannot FIRE.\n");
                    pthread_mutex_unlock(&rooms_mutex);
                    continue;
                }
                if (!room->gameStarted) {
                    send_to_client(client->socket, "Game not started yet.\n");
                    pthread_mutex_unlock(&rooms_mutex);
                    continue;
                }
                if (pIndex != room->current_turn) {
                    send_to_client(client->socket, "Not your turn!\n");
                    pthread_mutex_unlock(&rooms_mutex);
                    continue;
                }
                snprintf(msg, sizeof(msg), "%s\n", buffer);
                broadcast_to_room(room, msg, -1);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            else if (strncmp(buffer, "HIT ", 4) == 0) {
                snprintf(msg, sizeof(msg), "%s\n", buffer);
                broadcast_to_room(room, msg, -1);
                //room->current_turn = (room->current_turn == 0) ? 1 : 0; // Tutaj trzeba wrócić Miras
                if (room->clients[room->current_turn]) {
                    snprintf(msg, sizeof(msg), 
                             "NEXT_TURN %s TUTAJ POWINIEN ZOSTAC TEN SAM GRACZ\n",
                             room->clients[room->current_turn]->username);
                    broadcast_to_room(room, msg, -1);
                }
                send_board_update_to_observers(room);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            else if (strncmp(buffer, "MISS ", 5) == 0) {
                snprintf(msg, sizeof(msg), "%s\n", buffer);
                broadcast_to_room(room, msg, -1);
                room->current_turn = (room->current_turn == 0) ? 1 : 0;
                if (room->clients[room->current_turn]) {
                    snprintf(msg, sizeof(msg), "NEXT_TURN %s\n",
                             room->clients[room->current_turn]->username);
                    broadcast_to_room(room, msg, -1);
                }
                send_board_update_to_observers(room);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            else if (strncmp(buffer, "YOU_WIN", 7) == 0) {
                char winner[50];
                strncpy(winner, client->username, 49);
                winner[49] = '\0';
                snprintf(msg, sizeof(msg), "YOU_WIN %s\n", winner);
                broadcast_to_room(room, msg, -1);
                char loser[50] = "UNKNOWN";
                if (room->clients[0] && room->clients[0] != client) {
                    strncpy(loser, room->clients[0]->username, 49);
                } else if (room->clients[1] && room->clients[1] != client) {
                    strncpy(loser, room->clients[1]->username, 49);
                }
                log_game_result(winner, loser);
                if (room->clients[0]) {
                    room->clients[0]->room_id = -1;
                    send_to_client(room->clients[0]->socket, "Returning to lobby.\n");
                    send_to_client(room->clients[0]->socket, WELCOME_IN_LOBBY);
                    room->clients[0] = NULL;
                }
                if (room->clients[1]) {
                    room->clients[1]->room_id = -1;
                    send_to_client(room->clients[1]->socket, "Returning to lobby.\n");
                    send_to_client(room->clients[1]->socket, WELCOME_IN_LOBBY);
                    room->clients[1] = NULL;
                }
                for (int i = 0; i < room->observer_count; i++) {
                    if (room->observers[i]) {
                        room->observers[i]->room_id = -1;
                        send_to_client(room->observers[i]->socket, "Returning to lobby.\n");
                        send_to_client(room->observers[i]->socket, WELCOME_IN_LOBBY);
                        room->observers[i] = NULL;
                    }
                }
                room->observer_count = 0;
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            else {
                send_to_client(client->socket, "Invalid command in room.\n");
                pthread_mutex_unlock(&rooms_mutex);
            }
        }
    }

    // Obsługa rozłączenia klienta
    close(client->socket);
    client->active = 0;

    pthread_mutex_lock(&clients_mutex);
    pthread_mutex_lock(&rooms_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] == client) {
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j+1];
            }
            clients[client_count - 1] = NULL;
            client_count--;
            break;
        }
    }
    if (client->room_id != -1) {
        ChatRoom *r = get_room_by_id(client->room_id);
        if (r) {
            if (r->clients[0] == client)
                r->clients[0] = NULL;
            else if (r->clients[1] == client)
                r->clients[1] = NULL;
            else {
                for (int i = 0; i < r->observer_count; i++) {
                    if (r->observers[i] == client) {
                        for (int j = i; j < r->observer_count - 1; j++) {
                            r->observers[j] = r->observers[j+1];
                        }
                        r->observer_count--;
                        break;
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&rooms_mutex);
    pthread_mutex_unlock(&clients_mutex);

    free(client);
    pthread_exit(NULL);
}

// ==================== Funkcja main ====================
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <interface IP>\n", argv[0]);
        return 1;
    }
    char *interface_name = argv[1];

    #if RUN_AS_DAEMON
        daemonize();
    #endif

    signal(SIGINT, handle_sigint);

    // Uruchomienie wątku discovery UDP
    pthread_t disc_thread;
    pthread_create(&disc_thread, NULL, udp_discovery_thread, interface_name);
    pthread_detach(disc_thread);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("TCP socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d\n", SERVER_PORT);

    // Konfiguracja gniazda TLV (ephemeral port)
    tlv_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tlv_server_fd < 0) {
        perror("[TLV] socket creation failed");
        exit(EXIT_FAILURE);
    }
    setsockopt(tlv_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in tlv_addr;
    memset(&tlv_addr, 0, sizeof(tlv_addr));
    tlv_addr.sin_family = AF_INET;
    tlv_addr.sin_addr.s_addr = INADDR_ANY;
    tlv_addr.sin_port = htons(0);
    if (bind(tlv_server_fd, (struct sockaddr *)&tlv_addr, sizeof(tlv_addr)) < 0) {
        perror("[TLV] bind failed");
        close(tlv_server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(tlv_server_fd, 5) < 0) {
        perror("[TLV] listen failed");
        close(tlv_server_fd);
        exit(EXIT_FAILURE);
    }
    socklen_t len2 = sizeof(tlv_addr);
    getsockname(tlv_server_fd, (struct sockaddr *)&tlv_addr, &len2);
    tlv_port = ntohs(tlv_addr.sin_port);
    printf("[TLV] Listening on ephemeral port = %d\n", tlv_port);

    pthread_create(&tlv_thread, NULL, tlv_accept_thread, NULL);
    pthread_detach(tlv_thread);

    while (1) {
        printf("[DEBUG] Waiting for new connection...\n");
        Client *new_client = (Client *)malloc(sizeof(Client));
        socklen_t addr_len = sizeof(new_client->address);
        new_client->socket = accept(server_fd, (struct sockaddr *)&new_client->address, &addr_len);
        if (new_client->socket < 0) {
            perror("Accept failed");
            free(new_client);
            continue;
        }

        if (negotiate_username(new_client) < 0) {
            close(new_client->socket);
            free(new_client);
            continue;
        }

        new_client->room_id = -1;
        new_client->active = 1;
        new_client->tlv_socket = -1;

        pthread_mutex_lock(&clients_mutex);
        if (client_count < MAX_CLIENTS) {
            clients[client_count++] = new_client;
            pthread_t thread_id;
            pthread_create(&thread_id, NULL, handle_client, (void*)new_client);
            pthread_detach(thread_id);
            printf("New client connected: %s\n", new_client->username);
        } else {
            send_to_client(new_client->socket, "Server full.\n");
            close(new_client->socket);
            free(new_client);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    close(server_fd);
    return 0;
}
