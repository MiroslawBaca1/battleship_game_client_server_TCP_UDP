/*
 * Copyright (c) 2025 Miroslaw Baca & Marcel Gacoń
 * AGH - Programowanie sieciowe
 */

#ifndef GRA_H
#define GRA_H

/* ===================== Includy ===================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ===================== Definicje i Zmienne Globalne ===================== */
extern int server_socket;  // Globalny socket serwera (definiowany w klient.c)

#define BUFFER_SIZE    1024   // Rozmiar bufora wiadomości
static char msg[BUFFER_SIZE];     // Bufor do tworzenia komunikatów

#define BOARD_SIZE 8          // Rozmiar planszy (8x8)
// Znaki reprezentujące stany pól planszy
#define SHIP_CELL 'O'   // Mój statek
#define HIT_SHIP  'X'   // Trafiony statek
#define MISS_CELL '='   // Pudło
#define EMPTY_CELL '.'  // Puste pole

/* ===================== Globalne Zmienne Planszy ===================== */
// Tablice przechowujące stan mojej planszy oraz moje strzały
static char myBoard[BOARD_SIZE][BOARD_SIZE];  // Moja plansza (statki, trafienia przeciwnika)
static char myShots[BOARD_SIZE][BOARD_SIZE];  // Moje strzały w planszę przeciwnika

// Liczniki i flagi stanu
static int placedShips  = 0;   // Flaga: czy statki zostały rozstawione
static int myShipsCount = 0;   // Liczba fragmentów moich statków
static int myHitsCount  = 0;   // Liczba trafień wykonanych przez mnie
static int inRoom       = 0;   // Flaga: czy jestem w pokoju gry
static int iAmObserver  = 0;   // Flaga: czy jestem obserwatorem

static char username_received[50];  // Bufor do przechowywania odebranej nazwy użytkownika

int amFirstPlayer = -1;  // Określa rolę gracza: -1 = nieustalono, 1 = pierwszy gracz, 0 = drugi gracz

/* ===================== Inicjalizacja Planszy ===================== */
// Inicjalizuje obie tablice (myBoard i myShots) ustawiając wszystkie pola na EMPTY_CELL
static void initBoards() {
    placedShips  = 0;
    myShipsCount = 0;
    myHitsCount  = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            myBoard[i][j] = EMPTY_CELL;
            myShots[i][j] = EMPTY_CELL;
        }
    }
}

/* ===================== Wyświetlanie Plansz ===================== */
// Rysuje moją planszę – pokazuje moje statki, trafienia przeciwnika oraz pudła
static void printMyBoard() {
    printf("\n--- MY BOARD (O=ship, X=ship hit, %c=miss) ---\n  ", MISS_CELL);
    for (int j = 0; j < BOARD_SIZE; j++)
        printf("%d ", j);
    printf("\n");
    for (int i = 0; i < BOARD_SIZE; i++) {
        printf("%d ", i);
        for (int j = 0; j < BOARD_SIZE; j++) {
            printf("%c ", myBoard[i][j]);
        }
        printf("\n");
    }
    printf("\n");
}

// Rysuje tablicę moich strzałów w planszę przeciwnika
static void printMyShotsBoard() {
    printf("\n--- MY SHOTS BOARD (X=ship hit, %c=miss) ---\n  ", MISS_CELL);
    for (int j = 0; j < BOARD_SIZE; j++)
        printf("%d ", j);
    printf("\n");
    for (int i = 0; i < BOARD_SIZE; i++) {
        printf("%d ", i);
        for (int j = 0; j < BOARD_SIZE; j++) {
            printf("%c ", myShots[i][j]);
        }
        printf("\n");
    }
    printf("\n");
}

/* ===================== Walidacja Współrzędnych ===================== */
// Sprawdza, czy współrzędne (x, y) mieszczą się w zakresie planszy
static int validCoords(int x, int y)
{
    return (x >= 0 && x < BOARD_SIZE && y >= 0 && y < BOARD_SIZE);
}

/*
 * Czy wszystkie moje statki zostały trafione?
 */
static int allMyShipsAreHit()
{
    int hitCount = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            if (myBoard[i][j] == HIT_SHIP)
                hitCount++;
        }
    }
    return (hitCount >= myShipsCount);
}

/*
 * Kiedy przeciwnik strzela do mnie ("FIRE x y"), sprawdzam:
 *  - Jeśli myBoard[x][y] == 'O' => trafienie (ustaw 'X'),
 *  - W przeciwnym wypadku -> pudło (ustaw MISS_CELL).
 */
static int registerHitOrMiss(int x, int y)
{
    if (!validCoords(x, y))
        return -1;
    if (myBoard[x][y] == SHIP_CELL) {
        myBoard[x][y] = HIT_SHIP;
        return 1;
    }
    if (myBoard[x][y] == HIT_SHIP || myBoard[x][y] == MISS_CELL)
        return 0;
    myBoard[x][y] = MISS_CELL;
    return 0;
}

// Rejestruje wynik mojego strzału (trafienie lub pudło) w tablicy myShots
static void registerShotResult(int x, int y, int wasHit) {
    if (!validCoords(x, y))
        return;
    if (wasHit) {
        myShots[x][y] = HIT_SHIP;
        myHitsCount++;
    } else {
        myShots[x][y] = MISS_CELL;
    }
}

/* ===================== Spłaszczanie Planszy ===================== */
// Konwertuje dwuwymiarową tablicę myBoard do jednowymiarowego ciągu BOARD_SIZE*BOARD_SIZE znaków czyli w naszym wypadku 64 znaków + 1 znak na końcu czyli 65
static void flattenBoard(char flat[BOARD_SIZE*BOARD_SIZE+1]) {
    int index = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            flat[index++] = myBoard[i][j];
        }
    }
    flat[BOARD_SIZE*BOARD_SIZE] = '\0';
}

/* ===================== Wysyłanie Aktualizacji Planszy ===================== */
// Wysyła zaktualizowany stan planszy do serwera – w zależności od roli gracza (BOARD0 lub BOARD1)
void sendBoardUpdate() {
    char flat[BOARD_SIZE*BOARD_SIZE+1];
    flattenBoard(flat);
    if (amFirstPlayer == 1) {
        snprintf(msg, sizeof(msg), "BOARD0 %s\n", flat);
    } else if (amFirstPlayer == 0) {
        snprintf(msg, sizeof(msg), "BOARD1 %s\n", flat);
    } else {
        return;
    }
    if (send(server_socket, msg, strlen(msg), 0) < 0)
        perror("[CLIENT] Send board update failed");
    else
    {
        //WYSLANO POPRAWNIE
    }
}

/* ===================== Rozstawianie Statków ===================== */
// Umożliwia graczowi lokalne rozstawienie statków na planszy
static void placeShipsLocally() {
    if (placedShips) {
        printf("[PLACE] You have already placed your ships!\n");
        return;
    }
    printf("[PLACE] Place ships (horizontal)\n");
    #define shipNumber 2
    int shipLengths[shipNumber] = {1, 2};  // Można zmodyfikować na np. {3,3,2,2}
    for (int s = 0; s < shipNumber; s++) {
        int length = shipLengths[s];
        int placed = 0;
        printMyBoard();
        while (!placed) {
            printf("[PLACE] Place ship (length=%d) horizontally\n", length);
            printf("[PLACE] Enter row col (e.g. '2 3'): ");
            fflush(stdout);
            int x, y;
            if (scanf("%d %d", &x, &y) != 2) {
                int c;
                while ((c = getchar()) != '\n' && c != EOF) { }
                printf("[PLACE] Invalid input. Try again.\n");
                continue;
            }
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { }
            if (!validCoords(x, y) || !validCoords(x, y + length - 1)) {
                printf("[PLACE] Ship doesn't fit horizontally. Try again.\n");
                continue;
            }
            int conflict = 0;
            for (int k = 0; k < length; k++) {
                if (myBoard[x][y + k] != EMPTY_CELL) {
                    conflict = 1;
                    break;
                }
            }
            if (conflict) {
                printf("[PLACE] Collision with another ship. Try again.\n");
                continue;
            }
            for (int k = 0; k < length; k++) {
                myBoard[x][y + k] = SHIP_CELL;
                myShipsCount++;
            }
            placed = 1;
        }
    }
    placedShips = 1;
    printMyBoard();
    printf("[PLACE] All ships placed! Now type /start (once) to confirm readiness.\n");
}

/* ===================== Parsowanie Komunikatów ===================== */
// Przetwarza komunikaty otrzymywane od serwera i aktualizuje stan gry
static int parseBattleshipMessage(char *buffer, int *myTurn, int *gameStarted, const char *username) {
    if (!iAmObserver) {
        if (strncmp(buffer, "JOINED_ROOM", 11) == 0) {
            inRoom = 1;
            if (strncmp(buffer, "JOINED_ROOM_OBSERVER", 20) == 0) {
                iAmObserver = 1;
            }
            return 1;
        }
        else if (strncmp(buffer, "ENTERING_LOBBY", 14) == 0) {
            inRoom = 0;
            iAmObserver = 0;
            return 1;
        }
        else if (strncmp(buffer, "GAME_START", 10) == 0) {
            *gameStarted = 1;
            printf("[BATTLESHIP] GAME_START => The battle begins!\n");
            printMyBoard();
            printMyShotsBoard();
            return 1;
        }
        else if (strncmp(buffer, "NEXT_TURN ", 10) == 0) {
            char turnName[50];
            if (sscanf(buffer + 10, "%s", turnName) == 1) {
                if (strcmp(turnName, username) == 0) {
                    *myTurn = 1;
                    printf("[BATTLESHIP] It's now YOUR turn => /fire x y.\n");
                } else {
                    *myTurn = 0;
                    printf("[BATTLESHIP] It's now %s's turn. Please wait.\n", turnName);
                }
                sendBoardUpdate();
            }
            return 1;
        }
        else if (strncmp(buffer, "FIRE ", 5) == 0) {
            int x, y;
            if (sscanf(buffer + 5, "%d %d %s", &x, &y, username_received) == 3) {
                if (strcmp(username_received, username) != 0) {
                    int result = registerHitOrMiss(x, y);
                    if (result == 1) {
                        printf("[BATTLESHIP] Enemy HIT your ship at (%d,%d)\n", x, y);
                        if (allMyShipsAreHit()) {
                            snprintf(msg, sizeof(msg), "YOU_WIN");
                            send(server_socket, msg, strlen(msg), 0);
                        } else {
                            snprintf(msg, sizeof(msg), "HIT %d %d %s", x, y, username);
                            send(server_socket, msg, strlen(msg), 0);
                        }
                    } else {
                        printf("[BATTLESHIP] Enemy missed at (%d,%d)\n", x, y);
                        snprintf(msg, sizeof(msg), "MISS %d %d %s", x, y, username);
                        send(server_socket, msg, strlen(msg), 0);
                    }
                    printMyBoard();
                    printMyShotsBoard();
                }
            }
            return 1;
        }
        else if (strncmp(buffer, "HIT ", 4) == 0) {
            int x, y;
            char shooter[50];
            if (sscanf(buffer + 4, "%d %d %s", &x, &y, shooter) == 3) {
                if (strcmp(shooter, username) != 0) {
                    registerShotResult(x, y, 1);
                    printf("[BATTLESHIP] You HIT enemy at (%d,%d). Fire again!\n", x, y);
                    printMyBoard();
                    printMyShotsBoard();
                }
            }
            return 1;
        }
        else if (strncmp(buffer, "MISS ", 5) == 0) {
            int x, y;
            char shooter[50];
            if (sscanf(buffer + 5, "%d %d %s", &x, &y, shooter) == 3) {
                if (strcmp(shooter, username) != 0) {
                    registerShotResult(x, y, 0);
                    printf("[BATTLESHIP] You MISS at (%d,%d). Enemy's turn now.\n", x, y);
                    printMyBoard();
                    printMyShotsBoard();
                }
            }
            return 1;
        }
        else if (strncmp(buffer, "YOU_WIN", 7) == 0) {
            char winner[50];
            if (sscanf(buffer + 7, "%s", winner) == 1)
                printf("[BATTLESHIP] %s WON the game!\n", winner);
            else
                printf("[BATTLESHIP] Someone WON the game!\n");
            initBoards();
            return 1;
        }
    }
    else if (iAmObserver) {
        if (strncmp(buffer, "JOINED_ROOM", 11) == 0) {
            inRoom = 1;
            if (strncmp(buffer, "JOINED_ROOM_OBSERVER", 20) == 0) {
                iAmObserver = 1;
            }
            return 1;
        }
        else if (strncmp(buffer, "ENTERING_LOBBY", 14) == 0) {
            inRoom = 0;
            iAmObserver = 0;
            return 1;
        }
        else if (strncmp(buffer, "GAME_NOT_STARTED", 16) == 0) {
            printf("[BATTLESHIP][OBSERVER] The game hasn't started yet.\n");
            return 1;
        }
        else if (strncmp(buffer, "GAME_STARTED", 11) == 0) {
            printf("[BATTLESHIP][OBSERVER] The game is already in progress.\n");
            return 1;
        }
    }
    return 0;
}

#endif // GRA_H
