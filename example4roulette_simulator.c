#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

#define MAX_PLAYERS 100

typedef struct {
    int bet_amount;
    int chosen_number;
} BetMessage;

typedef struct {
    int player_money;
    int is_playing;
} PlayerState;

void player_process(int player_id, int initial_money, int write_fd) {
    int money = initial_money;
    srand(getpid() + time(NULL));

    printf("[%d]: I have %d and I'm going to play roulette.\n", getpid(), money);

    while (money > 0) {
        // 10% chance to leave with remaining money
        if (rand() % 100 < 10) {
            printf("[%d]: I saved %d and exit.\n", getpid(), money);
            break;
        }

        // Prepare bet
        BetMessage bet;
        bet.bet_amount = (rand() % money) + 1;  // Bet between 1 and money
        bet.chosen_number = rand() % 37;         // Number between 0-36

        // Send bet to dealer
        if (write(write_fd, &bet, sizeof(BetMessage)) == -1) {
            perror("Failed to send bet");
            exit(1);
        }

        // Receive result from dealer (0 = lost, 1 = won, 2 = game over)
        int result;
        if (read(STDIN_FILENO, &result, sizeof(int)) == -1) {
            perror("Failed to receive result");
            exit(1);
        }

        if (result == 2) {  // Game over signal
            break;
        } else if (result == 1) {  // Won
            money += bet.bet_amount * 35;
            printf("[%d]: I won %d.\n", getpid(), bet.bet_amount * 35);
        } else if (result == 0) {  // Lost
            money -= bet.bet_amount;
            if (money <= 0) {
                printf("[%d]: I'm broke and exit.\n", getpid());
                break;
            }
        }
    }

    close(write_fd);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <number of players> <starting money>\n", argv[0]);
        return 1;
    }

    int num_players = atoi(argv[1]);
    int starting_money = atoi(argv[2]);

    if (num_players < 1 || starting_money < 100) {
        fprintf(stderr, "Invalid arguments: N >= 1 and M >= 100\n");
        return 1;
    }

    srand(time(NULL) ^ getpid());

    // Create pipes for each player (player -> dealer communication)
    int player_pipes[MAX_PLAYERS][2];
    pid_t player_pids[MAX_PLAYERS];
    int active_players = num_players;

    // Create player processes
    for (int i = 0; i < num_players; i++) {
        if (pipe(player_pipes[i]) == -1) {
            perror("pipe");
            return 1;
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return 1;
        } else if (pid == 0) {
            // Child process (player)
            close(player_pipes[i][0]);  // Close read end
            player_process(i, starting_money, player_pipes[i][1]);
        } else {
            // Parent process (dealer)
            close(player_pipes[i][1]);  // Close write end
            player_pids[i] = pid;
        }
    }

    // Dealer main loop
    PlayerState player_states[MAX_PLAYERS];
    for (int i = 0; i < num_players; i++) {
        player_states[i].player_money = starting_money;
        player_states[i].is_playing = 1;
    }

    while (active_players > 0) {
        // Round: collect bets from all active players
        int bets_received = 0;
        BetMessage bets[MAX_PLAYERS];
        int bet_player_indices[MAX_PLAYERS];

        for (int i = 0; i < num_players; i++) {
            if (player_states[i].is_playing) {
                BetMessage bet;
                ssize_t bytes = read(player_pipes[i][0], &bet, sizeof(BetMessage));

                if (bytes == 0) {
                    // Player exited
                    player_states[i].is_playing = 0;
                    active_players--;
                } else if (bytes == sizeof(BetMessage)) {
                    bets[bets_received] = bet;
                    bet_player_indices[bets_received] = i;

                    // Get player PID for output
                    printf("Dealer: %d placed %d on %d\n", player_pids[i], bet.bet_amount, bet.chosen_number);
                    bets_received++;
                }
            }
        }

        if (bets_received == 0) {
            break;  // No more active players
        }

        // Draw lucky number
        int lucky_number = rand() % 37;
        printf("Dealer: %d is the lucky number.\n", lucky_number);

        // Determine winners and send results
        for (int i = 0; i < bets_received; i++) {
            int player_idx = bet_player_indices[i];
            int player_pid = player_pids[player_idx];
            BetMessage bet = bets[i];

            int result = 0;  // 0 = lost
            if (bet.chosen_number == lucky_number) {
                result = 1;  // 1 = won
                player_states[player_idx].player_money += bet.bet_amount * 35;
            } else {
                player_states[player_idx].player_money -= bet.bet_amount;
            }

            // Send result back to player
            if (write(player_pids[player_idx], &result, sizeof(int)) == -1) {
                perror("Failed to send result to player");
            }

            // Check if player is broke
            if (player_states[player_idx].player_money <= 0) {
                player_states[player_idx].is_playing = 0;
                active_players--;
            }
        }
    }

    // Wait for all player processes to finish
    for (int i = 0; i < num_players; i++) {
        waitpid(player_pids[i], NULL, 0);
    }

    printf("Dealer: Casino always wins\n");

    return 0;
}
