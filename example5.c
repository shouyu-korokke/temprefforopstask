/*Task
Write a program to simulate "My Ship Sails" using processes that communicate via pipes.
The program takes two arguments:
N - number of players (4 <= N <= 7)
M - number of cards per hand (M >= 4 and M ⋅ N <= 52)

Rules
Players are dealt M cards each from a 52-card deck (cards are represented as integers 0-51).
Each turn, players simultaneously pass a card to their right neighbor.
The game continues until a player collects M cards of the same suit and declares: My ship sails!.
The first to declare wins.
The suit of a card can be determined using % 4 operation.

Stages
Initialize:

The server process creates N player processes.
It shuffles the deck and deals M cards to each player via pipes.
Each player prints their received hand with their process ID and exits.
Gameplay:

Players form a ring, passing cards via pipes (nᵗʰ player → (n+1 % N)ᵗʰ player).
A player who collects M cards of the same suit prints [PID]: My ship sails! (game runs endlessly).
Winning Condition:

The server creates a shared pipe for winners to announce victory.
A player who wins writes their PID to the pipe, prints [PID]: My ship sails!, and exits.
The server reads the PID, prints Server: [PID] won!, and exits.
Termination:

Ctrl-C instantly stops all processes and cleans up resources.*/




#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_PLAYERS 7
#define DECK_SIZE 52

typedef struct {
    HANDLE read;
    HANDLE write;
} PipePair;

void shuffle(int *deck, int size) {
    srand(time(NULL));
    for (int i = size - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

int create_pipes(int N, PipePair *pipes, PipePair *winner_pipe) {
    if (!CreatePipe(&winner_pipe->read, &winner_pipe->write, NULL, 0)) {
        printf("Failed to create winner pipe\n");
        return 0;
    }
    for (int i = 0; i < N; i++) {
        if (!CreatePipe(&pipes[i].read, &pipes[i].write, NULL, 0)) {
            printf("Failed to create pipe %d\n", i);
            return 0;
        }
    }
    return 1;
}

void deal_cards(PipePair *pipes, int N, int M, int *deck) {
    int card_index = 0;
    for (int p = 0; p < N; p++) {
        for (int c = 0; c < M; c++) {
            DWORD written;
            WriteFile(pipes[p].write, &deck[card_index], sizeof(int), &written, NULL);
            card_index++;
        }
    }
}

int create_processes(int N, int M, PipePair *pipes, PipePair winner_pipe, char *argv0, PROCESS_INFORMATION *pi) {
    STARTUPINFO si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    for (int i = 0; i < N; i++) {
        char cmd[512];
        sprintf(cmd, "\"%s\" %d %d %d %lu %lu %lu", argv0, N, M, i, (unsigned long)pipes[i].read, (unsigned long)pipes[(i+1)%N].write, (unsigned long)winner_pipe.write);
        if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi[i])) {
            printf("Failed to create process %d\n", i);
            return 0;
        }
        CloseHandle(pi[i].hThread);
    }
    return 1;
}

void run_server(int N, int M, char *argv0) {
    PipePair pipes[MAX_PLAYERS];
    PipePair winner_pipe;
    PROCESS_INFORMATION pi[MAX_PLAYERS];

    if (!create_pipes(N, pipes, &winner_pipe)) return;

    int deck[DECK_SIZE];
    for (int i = 0; i < DECK_SIZE; i++) deck[i] = i;
    shuffle(deck, DECK_SIZE);

    deal_cards(pipes, N, M, deck);

    if (!create_processes(N, M, pipes, winner_pipe, argv0, pi)) return;

    // Close unnecessary handles
    for (int i = 0; i < N; i++) {
        CloseHandle(pipes[i].read);
        CloseHandle(pipes[i].write);
    }
    CloseHandle(winner_pipe.write);

    // Wait for winner
    DWORD read_bytes;
    int winner_pid;
    if (ReadFile(winner_pipe.read, &winner_pid, sizeof(int), &read_bytes, NULL)) {
        printf("Server: %d won!\n", winner_pid);
    }

    // Close handles
    CloseHandle(winner_pipe.read);
    for (int i = 0; i < N; i++) {
        CloseHandle(pi[i].hProcess);
    }
}

void run_player(int N, int M, int id, HANDLE input, HANDLE output, HANDLE winner_write) {
    int hand[DECK_SIZE];
    int hand_size = 0;
    for (int i = 0; i < M; i++) {
        DWORD read;
        if (!ReadFile(input, &hand[hand_size], sizeof(int), &read, NULL)) {
            printf("Failed to read card\n");
            return;
        }
        hand_size++;
    }

    printf("[%lu]: ", GetCurrentProcessId());
    for (int i = 0; i < hand_size; i++) printf("%d ", hand[i]);
    printf("\n");

    srand(time(NULL) + id);

    // Initial pass
    int pass_index = rand() % hand_size;
    int pass_card = hand[pass_index];
    hand[pass_index] = hand[--hand_size];
    DWORD written;
    if (!WriteFile(output, &pass_card, sizeof(int), &written, NULL)) {
        printf("Failed to write card\n");
        return;
    }

    while (1) {
        // Read card
        int card;
        DWORD read;
        if (!ReadFile(input, &card, sizeof(int), &read, NULL)) {
            printf("Failed to read card\n");
            return;
        }
        hand[hand_size++] = card;

        // Check win
        int suit_count[4] = {0};
        for (int i = 0; i < hand_size; i++) suit_count[hand[i] % 4]++;
        int win = 0;
        for (int s = 0; s < 4; s++) if (suit_count[s] == M) win = 1;
        if (win) {
            int pid = GetCurrentProcessId();
            if (!WriteFile(winner_write, &pid, sizeof(int), &written, NULL)) {
                printf("Failed to write winner\n");
                return;
            }
            printf("[%d]: My ship sails!\n", pid);
            return;
        }

        // Pass card
        pass_index = rand() % hand_size;
        pass_card = hand[pass_index];
        hand[pass_index] = hand[--hand_size];
        if (!WriteFile(output, &pass_card, sizeof(int), &written, NULL)) {
            printf("Failed to write card\n");
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc == 3) {
        // Server mode
        int N = atoi(argv[1]);
        int M = atoi(argv[2]);
        if (N < 4 || N > 7 || M < 4 || M * N > 52) {
            printf("Invalid N or M\n");
            return 1;
        }

        run_server(N, M, argv[0]);
        return 0;
    } else if (argc == 7) {
        // Player mode
        int N = atoi(argv[1]);
        int M = atoi(argv[2]);
        int id = atoi(argv[3]);
        HANDLE input = (HANDLE)atol(argv[4]);
        HANDLE output = (HANDLE)atol(argv[5]);
        HANDLE winner_write = (HANDLE)atol(argv[6]);

        run_player(N, M, id, input, output, winner_write);
    } else {
        printf("Usage: %s N M\n", argv[0]);
        return 1;
    }
}
