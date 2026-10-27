#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#include "protocol.h"
#include "common.h"
#include "err.h"

#define MAX_RECV_BUFFER 512

// --- HELPER FUNCTIONS ---

// Safely converts a string to an unsigned integer, handling conversion errors
static uint32_t safe_strtoul(const char *str, const char *err_msg) {
    if (!str) fatal("%s", err_msg);

    // Prevent negative numbers from wrapping around to large positive integers
    if (str[0] == '-') fatal("Negative numbers not allowed: %s", err_msg);

    char *endptr;
    errno = 0;
    unsigned long val = strtoul(str, &endptr, 10);

    if (errno != 0 || *endptr != '\0' || str == endptr) {
        fatal("Invalid number format: %s", err_msg);
    }
    return (uint32_t)val;
}

// Parses command-line arguments and validates them
static void parse_arguments(int argc, char *argv[], char **address_str,
                            uint16_t *port, char **message_str,
                            int *client_timeout) {
    int opt;
    bool has_a = false, has_p = false, has_m = false, has_t = false;

    while ((opt = getopt(argc, argv, "a:p:m:t:")) != -1) {
        switch (opt) {
            case 'a':
                *address_str = optarg;
                has_a = true;
                break;
            case 'p':
                *port = read_port(optarg);
                has_p = true;
                break;
            case 'm':
                *message_str = optarg;
                has_m = true;
                break;
            case 't': {
                char *endptr;
                errno = 0;
                long val = strtol(optarg, &endptr, 10);
                if (errno != 0 || *endptr != '\0' || optarg == endptr ||
                    val < 1 || val > 99) {
                    fatal("Client timeout must be an integer between 1 and 99");
                }
                *client_timeout = (int)val;
                has_t = true;
                break;
            }
            default:
                usage(argv[0]);
        }
    }

    if (!has_a || !has_p || !has_m || !has_t) usage(argv[0]);
    if (*port == 0) fatal("Client port must be between 1 and 65535");
}

// Parses the complex message string (e.g., "1/42/5/3") into separate variables
static void parse_message(const char *message_str, uint8_t *msg_type,
                          uint32_t *player_id, uint32_t *game_id,
                          uint8_t *pawn) {
    // Copy the string because strtok modifies the original buffer
    char msg_copy[256];
    strncpy(msg_copy, message_str, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';

    // Parse the mandatory fields (msg_type and player_id)
    char *token = strtok(msg_copy, "/");
    *msg_type = (uint8_t)safe_strtoul(token, "Missing/invalid msg_type");

    token = strtok(NULL, "/");
    *player_id = safe_strtoul(token, "Missing/invalid player_id");

    // Parse the conditional fields depending on the message type
    switch (*msg_type) {
        case MSG_JOIN:
            break;
        case MSG_MOVE_1:
        case MSG_MOVE_2:
            token = strtok(NULL, "/");
            *game_id = safe_strtoul(token, "Missing/invalid game_id");
            token = strtok(NULL, "/");
            *pawn = (uint8_t)safe_strtoul(token, "Missing/invalid pawn");
            break;
        case MSG_KEEP_ALIVE:
        case MSG_GIVE_UP:
            token = strtok(NULL, "/");
            *game_id = safe_strtoul(token, "Missing/invalid game_id");
            break;
        default:
            fatal("Unknown message type: %d", *msg_type);
    }

    // Ensure no garbage is appended at the end (e.g. "0/10/20")
    if (strtok(NULL, "/") != NULL) {
        fatal("Invalid format: too many parameters in -m");
    }
}

// Assembles the binary UDP packet according to the protocol struct
static size_t build_packet(uint8_t *buffer, uint8_t msg_type,
                           uint32_t player_id, uint32_t game_id, uint8_t pawn) {
    size_t packet_size = 0;

    switch (msg_type) {
        case MSG_JOIN: {
            client_msg_join_t *msg = (client_msg_join_t *)buffer;
            msg->msg_type = msg_type;
            msg->player_id = htonl(player_id); // Convert to Network Order
            packet_size = sizeof(client_msg_join_t);
            break;
        }
        case MSG_MOVE_1:
        case MSG_MOVE_2: {
            client_msg_move_t *msg = (client_msg_move_t *)buffer;
            msg->msg_type = msg_type;
            msg->player_id = htonl(player_id);
            msg->game_id = htonl(game_id);
            msg->pawn = pawn;
            packet_size = sizeof(client_msg_move_t);
            break;
        }
        case MSG_KEEP_ALIVE:
        case MSG_GIVE_UP: {
            client_msg_basic_t *msg = (client_msg_basic_t *)buffer;
            msg->msg_type = msg_type;
            msg->player_id = htonl(player_id);
            msg->game_id = htonl(game_id);
            packet_size = sizeof(client_msg_basic_t);
            break;
        }
    }
    return packet_size;
}

// Renders the game state visually in the console
static void print_game_state(server_game_state_t *state) {
    printf("\n==== GAME STATE (Game ID: %u) ====\n", ntohl(state->game_id));
    printf("Player A (ID): %u\n", ntohl(state->player_a_id));
    printf("Player B (ID): %u\n", ntohl(state->player_b_id));

    printf("Status:       ");
    switch (state->status) {
        case 0: printf("WAITING_FOR_OPPONENT (Waiting for Player B)\n"); break;
        case 1: printf("TURN_A (Player A's turn)\n"); break;
        case 2: printf("TURN_B (Player B's turn)\n"); break;
        case 3: printf("WIN_A (Player A wins!)\n"); break;
        case 4: printf("WIN_B (Player B wins!)\n"); break;
        default: printf("UNKNOWN (%d)\n", state->status); break;
    }

    printf("\nBoard:\n");

    // Print index numbers above the pawns for clarity
    for (int i = 0; i <= state->max_pawn; i++) {
        printf("%2d ", i);
    }
    printf("\n");

    // Print the standing pawns using bitwise operations
    for (int i = 0; i <= state->max_pawn; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        bool is_standing = (state->pawn_row[byte_idx] & (1 << bit_idx)) != 0;

        if (is_standing) printf("[I]");
        else printf("   ");
    }
    printf("\n================================\n\n");
}

// --- MAIN FUNCTION ---

int main(int argc, char *argv[]) {
    char *address_str = NULL;
    uint16_t port = 0;
    char *message_str = NULL;
    int client_timeout = 0;

    // 1. Parse arguments and the specific message (-m)
    parse_arguments(argc, argv, &address_str, &port, &message_str,
                    &client_timeout);

    uint8_t msg_type = 0;
    uint32_t player_id = 0, game_id = 0;
    uint8_t pawn = 0;
    parse_message(message_str, &msg_type, &player_id, &game_id, &pawn);

    // 2. Build the UDP packet
    uint8_t send_buffer[64];
    size_t packet_size = build_packet(send_buffer, msg_type, player_id,
                                      game_id, pawn);

    // 3. Socket configuration
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) syserr("cannot create a socket");

    struct sockaddr_in server_address = get_server_address(address_str, port);

    // Apply the client timeout to the receiving socket
    struct timeval to;
    to.tv_sec = client_timeout;
    to.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to)) < 0) {
        fatal("setsockopt SO_RCVTIMEO failed");
    }

    // 4. Communication
    ssize_t sent_length = sendto(socket_fd, send_buffer, packet_size, 0,
                                 (struct sockaddr *)&server_address,
                                 sizeof(server_address));
    if (sent_length != (ssize_t)packet_size) syserr("incomplete sending");

    uint8_t recv_buffer[MAX_RECV_BUFFER];
    ssize_t received_length = recvfrom(socket_fd, recv_buffer,
                                       sizeof(recv_buffer), 0, NULL, NULL);

    // Handle timeout gracefully (Exit code 0 required by spec)
    if (received_length < 0) {
        printf("No response from server (Timeout: %d sec).\n", client_timeout);
        close(socket_fd);
        return 0;
    }

    // 5. Parse response
    if (received_length >= 13 && recv_buffer[12] == 255) {
        printf("\n--- MESSAGE REJECTED ---\n");
        printf("The server rejected your packet (MSG_WRONG_MSG).\n");
        if (received_length >= 14) {
            printf("Error at byte index: %d\n", recv_buffer[13]);
        }
        close(socket_fd);
        return 0;
    }

    if (received_length < (ssize_t)sizeof(server_game_state_t)) {
        printf("Received incomplete game state packet.\n");
        close(socket_fd);
        return 0;
    }

    print_game_state((server_game_state_t *)recv_buffer);

    close(socket_fd);
    return 0;
}