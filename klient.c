/*
 * Copyright (c) 2025 Miroslaw Baca & Marcel Gacoń
 * AGH - Programowanie sieciowe
 */
 
/* ===================== Includy i Definicje ===================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/in.h>

#include "gra.h"  // Logika gry w statki

int server_socket;  // Definicja globalnego socketu serwera (extern zadeklarowany w gra.h)

#define BUFFER_SIZE    1024
#define DISCOVERY_PORT 12346
#define MULTICAST_ADDR "239.255.0.1"
#define DEF_SERVER_PORT 12345

/* ===================== Zmienne Globalne ===================== */
char username[50];
int running = 1;

pthread_t receive_thread;

// Flagi stanu gry
int gameStarted = 0;
int myTurn      = 0;
int iAmReady    = 0;

// Globalny adres serwera – wykorzystywany przy łączeniu kanałem TLV
char global_server_ip[100];

// Zmienne związane z obsługą TLV
int tlv_socket = -1;         // Socket do komunikacji TLV
pthread_t tlv_receive_thread;  // Wątek odbierający dane TLV

/* ===================== Obsługa TLV ===================== */

// Funkcja pomocnicza do wyświetlania planszy 8x8 otrzymanej przez TLV
static void displayBoardData(const unsigned char *data, int length, const char *label) {
    if (length != 64) {
        printf("[TLV] Nieoczekiwana długość planszy: %d bajtów\n", length);
        return;
    }
    printf("\n[%s]\n", label);
    printf("   ");
    for (int i = 0; i < 8; i++) {
        printf("%d ", i);
    }
    printf("\n");
    for (int i = 0; i < 8; i++) {
        printf("%d  ", i);
        for (int j = 0; j < 8; j++) {
            printf("%c ", data[i * 8 + j]);
        }
        printf("\n");
    }
    printf("\n");
}

// Wątek odbierający dane TLV i prezentujący je jako planszę
static void *receive_tlv_messages(void *arg) {
    (void)arg; // Nieużywany argument
    unsigned char tlv_buf[67];
    int n;
    while ((n = recv(tlv_socket, tlv_buf, sizeof(tlv_buf), 0)) > 0) {
        for (int i = 0; i < n; i++) {
            printf("%02X ", tlv_buf[i]);
        }
        printf("\n");

        // Sprawdzamy czy długość nagłówka jest odpowiednia, jak nie to pomijamy (3 bajty)
        if (n < 3) {
            continue;
        }
        unsigned char type = tlv_buf[0];
        int length = (tlv_buf[1] << 8) | tlv_buf[2];

        // Weryfikujemy, czy cały pakiet (nagłówek + dane) został odebrany a jak nie to czekamy na więcej danych
        if (n < 3 + length) {
            continue;
        }

        // Ustalenie etykiety planszy w zależności od typu pakietu
        const char *boardLabel = NULL;
        if (type == 0x01) {
            boardLabel = "Plansza Gracza 1";
        } else if (type == 0x02) {
            boardLabel = "Plansza Gracza 2";
        } else {
            boardLabel = "Nieznany typ TLV";
        }

        // Wyświetlenie odebranych danych jako plansza 8x8
        displayBoardData(tlv_buf + 3, length, boardLabel);
    }
    close(tlv_socket);
    return NULL;
}

/* ===================== Multicast Discovery ===================== */

// Funkcja wysyłająca DISCOVERY_REQUEST i odbierająca odpowiedź z serwera
int discover_server(char *out_ip, int *out_port, const char *interface_name) {
    int udp_sock;
    struct sockaddr_in addr;
    char buffer[BUFFER_SIZE];
    struct timeval tv;

    // Utworzenie socketu UDP
    if ((udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("[DISCOVERY] socket UDP");
        return -1;
    }

    struct in_addr local_interface;
    if (inet_aton(interface_name, &local_interface) == 0) {
        perror("[DISCOVERY] Invalid interface IP");
        close(udp_sock);
        return -1;
    }
    if (setsockopt(udp_sock, IPPROTO_IP, IP_MULTICAST_IF, &local_interface, sizeof(local_interface)) < 0) {
        perror("[DISCOVERY] setsockopt IP_MULTICAST_IF");
        close(udp_sock);
        return -1;
    }

    // Ustawienie timeoutu na 3 sekundy
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(MULTICAST_ADDR);
    addr.sin_port        = htons(DISCOVERY_PORT);

    int ttl = 5;
    setsockopt(udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    printf("[DISCOVERY] Sending DISCOVERY_REQUEST to %s:%d\n", MULTICAST_ADDR, DISCOVERY_PORT);

    const char *request = "DISCOVERY_REQUEST";
    if (sendto(udp_sock, request, strlen(request), 0, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[DISCOVERY] sendto");
        close(udp_sock);
        return -1;
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int n = recvfrom(udp_sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&from_addr, &from_len);
    if (n < 0) {
        printf("[DISCOVERY] No response (timeout?).\n");
        close(udp_sock);
        return -1;
    }
    buffer[n] = '\0';

    if (strncmp(buffer, "SERVER_IP=", 9) == 0) {
        char *p = buffer + 10;  // wskazuje na "X.X.X.X:port"
        char *colon = strstr(p, ":");
        if (!colon) {
            printf("[DISCOVERY] Invalid response format.\n");
            close(udp_sock);
            return -1;
        }
        *colon = '\0';
        colon++;
        strcpy(out_ip, p);
        *out_port = atoi(colon);
        close(udp_sock);
        return 0; // Sukces
    }

    printf("[DISCOVERY] Unknown response: %s\n", buffer);
    close(udp_sock);
    return -1;
}

/* ===================== Obsługa Odbioru Wiadomości ===================== */

// Wątek odbierający wiadomości tekstowe z serwera (line-based)
void *receive_messages(void *arg) {
    static char lineBuf[4096];
    static int lineLen = 0;
    char tmp[512];
    while (running) {
        int n = recv(server_socket, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            printf("Disconnected.\n");
            running = 0;
            break;
        }
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            lineBuf[lineLen++] = c;
            // Jeśli natrafimy na znak nowej linii, kończymy buforowanie
            if (c == '\n') {
                lineBuf[lineLen-1] = '\0';
                // Obsługa komunikatu TLV_PORT – inicjujemy oddzielne połączenie TLV
                if (strncmp(lineBuf, "TLV_PORT ", 9) == 0) {
                    int tlv_port = atoi(lineBuf + 9);
                    printf("[TLV] Received TLV port: %d\n", tlv_port);
                    struct sockaddr_in tlv_addr;
                    tlv_socket = socket(AF_INET, SOCK_STREAM, 0);
                    if (tlv_socket < 0) {
                        perror("[TLV] TCP socket creation failed");
                    } else {
                        memset(&tlv_addr, 0, sizeof(tlv_addr));
                        tlv_addr.sin_family = AF_INET;
                        tlv_addr.sin_port = htons(tlv_port);
                        if (inet_pton(AF_INET, global_server_ip, &tlv_addr.sin_addr) <= 0) {
                            perror("[TLV] Invalid server address for TLV");
                        } else {
                            if (connect(tlv_socket, (struct sockaddr *)&tlv_addr, sizeof(tlv_addr)) < 0) {
                                perror("[TLV] Connection to TLV channel failed");
                            } else {
                                if (send(tlv_socket, username, strlen(username), 0) < 0) {
                                    perror("[TLV] Failed to send TLV username");
                                }
                                printf("[TLV] Connected to TLV channel.\n");
                                pthread_create(&tlv_receive_thread, NULL, receive_tlv_messages, NULL);
                                pthread_detach(tlv_receive_thread);
                            }
                        }
                    }
                    lineLen = 0;
                    continue;
                }
                // Parsujemy komunikaty dotyczące gry
                if (!parseBattleshipMessage(lineBuf, &myTurn, &gameStarted, username)) {
                    printf("%s\n", lineBuf);
                }
                lineLen = 0;
            }
            else if (lineLen >= 4095) {
                lineBuf[lineLen] = '\0';
                if (!parseBattleshipMessage(lineBuf, &myTurn, &gameStarted, username)) {
                    printf("%s\n", lineBuf);
                }
                lineLen = 0;
            }
        }
    }
    return NULL;
}

/* ===================== Handshake ===================== */

// Funkcja blokująca odczyt jednej linii z socketu (używana podczas handshake)
static int read_line_blocking(int sock, char *buf, int bufsize) {
    int idx = 0;
    while (idx < bufsize - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0)
            return -1;
        if (c == '\n') {
            buf[idx] = '\0';
            return idx;
        }
        buf[idx++] = c;
    }
    buf[idx] = '\0';
    return idx;
}

// Funkcja negocjująca nazwę użytkownika – czeka na "Enter your username:" i wysyła nick
int handshake_username(int sockfd) {
    char line[BUFFER_SIZE];
    while (1) {
        int n = read_line_blocking(sockfd, line, sizeof(line));
        if (n <= 0) {
            printf("[CLIENT] Connection closed or error during handshake.\n");
            return -1;
        }
        if (strncmp(line, "Enter your username:", 20) == 0) {
            printf("%s\n", line);
            printf("Your username: ");
            fflush(stdout);
            if (!fgets(username, sizeof(username), stdin))
                return -1;
            username[strcspn(username, "\n")] = '\0';
            if (send(sockfd, username, strlen(username), 0) < 0)
                return -1;
        }
        else if (strncmp(line, "Username in use", 15) == 0) {
            printf("%s\n", line);
        }
        else if (strncmp(line, "Username cannot be empty", 24) == 0) {
            printf("%s\n", line);
        }
        else if (strncmp(line, "Username accepted", 17) == 0) {
            printf("%s\n", line);
            return 0;
        }
        else {
            printf("%s\n", line);
        }
    }
    return 0;
}

/* ===================== Funkcja main ===================== */

int main(int argc, char *argv[]) {
    char server_ip[100];
    int server_port = DEF_SERVER_PORT;  // Port domyślny

    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <interface IP>                 (automatic discovery)\n", argv[0]);
        fprintf(stderr, "  %s --serverIP <server IP>        (direct connect)\n", argv[0]);
        return 1;
    }

    // Przetwarzanie argumentów wiersza poleceń
    if (strcmp(argv[1], "--serverIP") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Missing <server IP> argument.\n");
            return 1;
        }
        strcpy(server_ip, argv[2]);
    } else {
        const char *interface_name = argv[1];
        printf("[DISCOVERY] Attempting to find server via multicast...\n");
        if (discover_server(server_ip, &server_port, interface_name) < 0) {
            printf("[DISCOVERY] Failed. Exiting.\n");
            return 1;
        }
    }
    // Ustawienie globalnego adresu serwera (do TLV)
    strcpy(global_server_ip, server_ip);

    printf("[INFO] Connecting to server at %s:%d\n", server_ip, server_port);

    struct sockaddr_in server_address;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("[CLIENT] TCP socket creation failed");
        exit(EXIT_FAILURE);
    }
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port   = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_address.sin_addr) <= 0) {
        perror("[CLIENT] Invalid server address");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    if (connect(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("[CLIENT] Connection failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    if (handshake_username(server_socket) < 0) {
        close(server_socket);
        return 1;
    }
    initBoards();
    pthread_create(&receive_thread, NULL, receive_messages, NULL);

    char message[BUFFER_SIZE];
    while (running) {
        if (!fgets(message, sizeof(message), stdin))
            break;
        message[strcspn(message, "\n")] = '\0';

        if (message[0] == '/') {
            // Obsługa komend wysyłanych przez klienta
            if (strncmp(message, "/exit", 5) == 0) {
                if (send(server_socket, message, strlen(message), 0) < 0)
                    printf("[CLIENT] Send error.\n");
                continue;
            }
            else if (strncmp(message, "/create", 7) == 0) {
                // Klient tworzący pokój jest pierwszym graczem
                amFirstPlayer = 1;
                if (send(server_socket, message, strlen(message), 0) < 0)
                    printf("[CLIENT] Send error.\n");
                continue;
            }
            else if (strncmp(message, "/join ", 6) == 0) {
                // Klient dołączający do pokoju jako drugi gracz
                amFirstPlayer = 0;
                if (send(server_socket, message, strlen(message), 0) < 0)
                    printf("[CLIENT] Send error.\n");
                continue;
            }
            else if (strncmp(message, "/place", 6) == 0) {
                if (inRoom && !iAmObserver) {
                    placeShipsLocally();
                    sendBoardUpdate();  // Wysyłamy stan planszy po rozstawieniu statków
                } else if (iAmObserver) {
                    printf("Observer cannot /place.\n");
                } else {
                    printf("[WARNING] You can use /place only in game room!\n");
                }
                continue;
            }
            else if (strncmp(message, "/start", 6) == 0) {
                if (!placedShips)
                    printf("[BATTLESHIP] You must /place your ships first!\n");
                if (iAmReady)
                    printf("[BATTLESHIP] You have already used /start!\n");
                if (gameStarted)
                    printf("[BATTLESHIP] Game has already started.\n");
                iAmReady = 1;
                if (send(server_socket, "/start", 6, 0) < 0)
                    printf("[CLIENT] Send error.\n");
                continue;
            }
            else if (strncmp(message, "/fire ", 6) == 0) {
                if (!gameStarted) {
                    printf("[BATTLESHIP] Game not started.\n");
                    continue;
                }
                if (!myTurn) {
                    printf("[BATTLESHIP] It's not your turn!\n");
                    continue;
                }
                int x, y;
                if (sscanf(message + 6, "%d %d", &x, &y) == 2) {
                    if (!validCoords(x, y)) {
                        printf("[BATTLESHIP] Invalid coords.\n");
                        continue;
                    }
                    char msg_to_send[BUFFER_SIZE];
                    snprintf(msg_to_send, sizeof(msg_to_send), "FIRE %d %d %s", x, y, username);
                    if (send(server_socket, msg_to_send, strlen(msg_to_send), 0) < 0)
                        printf("[CLIENT] Send error.\n");
                } else {
                    printf("[BATTLESHIP] Usage: /fire x y\n");
                }
                continue;
            }
            // Wysyłanie pozostałych komend bez modyfikacji
            if (send(server_socket, message, strlen(message), 0) < 0)
                printf("[CLIENT] Send error.\n");
        } else {
            if (send(server_socket, message, strlen(message), 0) < 0) {
                printf("[CLIENT] Send error.\n");
                break;
            }
        }
    }
    running = 0;
    pthread_cancel(receive_thread);
    pthread_join(receive_thread, NULL);
    close(server_socket);
    if (tlv_socket != -1)
        close(tlv_socket);
    printf("[CLIENT] Terminated.\n");
    return 0;
}
