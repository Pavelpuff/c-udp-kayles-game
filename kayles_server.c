#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <time.h>

#include "kayles.h"
#include "err.h"
#include "common.h"
#include "protocol.h"

// Constants for UDP and game limits
#define MAX_CLIENT_MSG_SIZE 80
#define SOCK_TIMEOUT 500000 // 0.5 seconds in microseconds
#define MAX_ACTIVE_GAMES 1024

// Constants for MSG_WRONG_MSG offsets according to protocol
#define WRONG_MSG_STATUS_OFFSET 12
#define WRONG_MSG_ERROR_INDEX_OFFSET 13
#define WRONG_MSG_TOTAL_LEN 14

// --- STRUCTURES & GLOBALS ---

// Represents a single game session in the server memory
typedef struct {
    bool is_taken;                      // Is this slot active?
    struct sockaddr_in client_addr_a;   // Network address of Player A
    struct sockaddr_in client_addr_b;   // Network address of Player B
    socklen_t addr_len_a;
    socklen_t addr_len_b;
    time_t last_msg_a;                  // Timestamp of the last valid message
    time_t last_msg_b;
    time_t end_game_timer;              // Timer for post-game retention
    server_game_state_t *game;          // Pointer to the actual game state
} game_session_t;

// Object Pool for O(1) memory allocation (no malloc in the main loop)
static game_session_t game_table[MAX_ACTIVE_GAMES];
static int free_indices[MAX_ACTIVE_GAMES];
static int free_count = MAX_ACTIVE_GAMES;
static uint32_t game_id_counter = 1;

// Template variables for fast game initialization
static uint8_t *template_pawn_row = NULL;
static uint8_t template_max_pawn = 0;
static size_t template_row_size = 0;
static int server_timeout = 0;

// --- NETWORK UTILITIES ---

// Serializes and sends the current game state to a specific client
static void send_game_state(int socket_fd, game_session_t *session,
                            struct sockaddr_in *client_addr,
                            socklen_t address_len) {
    size_t packet_size = sizeof(server_game_state_t) + template_row_size;

    // Use a flat buffer to safely handle the Flexible Array Member (pawn_row)
    uint8_t response_buffer[1024] = {0};
    memcpy(response_buffer, session->game, packet_size);

    // Convert multi-byte integers to Network Byte Order
    server_game_state_t *response = (server_game_state_t *)response_buffer;
    response->game_id = htonl(session->game->game_id);
    response->player_a_id = htonl(session->game->player_a_id);
    response->player_b_id = htonl(session->game->player_b_id);

    sendto(socket_fd, response_buffer, packet_size, 0,
           (struct sockaddr *)client_addr, address_len);
}

// Sends an error message (MSG_WRONG_MSG) when a bad packet is received
static void send_wrong_msg(int socket_fd, uint8_t *orig_msg, ssize_t orig_len,
                           uint8_t err_idx, struct sockaddr_in *client_addr,
                           socklen_t address_len) {
    uint8_t response[WRONG_MSG_TOTAL_LEN] = {0};

    // Copy up to 12 bytes of the original bad message
    size_t copy_len = (orig_len < 12) ? (size_t)orig_len : 12;
    memcpy(response, orig_msg, copy_len);

    // Append the error status and index
    response[WRONG_MSG_STATUS_OFFSET] = STATUS_WRONG_MSG;
    response[WRONG_MSG_ERROR_INDEX_OFFSET] = err_idx;

    sendto(socket_fd, response, WRONG_MSG_TOTAL_LEN, 0,
           (struct sockaddr *)client_addr, address_len);
}

// --- MEMORY & GAME MANAGEMENT ---

// Finds an active game session by its unique ID
static game_session_t *find_game_session(uint32_t target_game_id) {
    for (int i = 0; i < MAX_ACTIVE_GAMES; ++i) {
        if (game_table[i].is_taken &&
            game_table[i].game->game_id == target_game_id) {
            return &game_table[i];
        }
    }
    return NULL;
}

// Handles player connections (MSG_JOIN) and creates/joins games
static game_session_t *handle_client_join(uint32_t player_id,
                                          struct sockaddr_in *client_addr,
                                          socklen_t address_len) {
    time_t now = time(NULL);

    // Step 1: Look for an existing game waiting for Player B
    for (int i = 0; i < MAX_ACTIVE_GAMES; ++i) {
        if (game_table[i].is_taken &&
            game_table[i].game->status == STATUS_WAITING_FOR_OPPONENT) {
            game_session_t *session = &game_table[i];

            // Player B joins. According to spec tests, B moves first!
            session->game->player_b_id = player_id;
            session->game->status = STATUS_TURN_B;
            session->client_addr_b = *client_addr;
            session->addr_len_b = address_len;
            session->last_msg_b = now;
            return session;
        }
    }

    // Step 2: No waiting games found. Check if the server is full.
    if (free_count == 0) return NULL;

    // Step 3: Create a new game in O(1) using the free index stack
    int idx = free_indices[--free_count];
    game_session_t *session = &game_table[idx];

    // Allocate memory matching the structure + flexible array size
    session->game = malloc(sizeof(server_game_state_t) + template_row_size);
    if (!session->game) fatal("Out of memory during session creation");

    session->is_taken = true;
    session->client_addr_a = *client_addr;
    session->addr_len_a = address_len;
    session->last_msg_a = now;
    session->end_game_timer = 0;

    // Initialize game state based on the template
    session->game->game_id = game_id_counter++;
    session->game->player_a_id = player_id;
    session->game->player_b_id = 0;
    session->game->status = STATUS_WAITING_FOR_OPPONENT;
    session->game->max_pawn = template_max_pawn;
    memcpy(session->game->pawn_row, template_pawn_row, template_row_size);

    return session;
}

// Garbage Collector: removes inactive players and cleans up finished games
static void check_and_cleanup_expired_games() {
    time_t now = time(NULL);

    for (int i = 0; i < MAX_ACTIVE_GAMES; ++i) {
        if (!game_table[i].is_taken) continue;

        game_session_t *session = &game_table[i];
        server_game_state_t *game = session->game;

        // Condition 1: Clean up finished games after retention timer expires
        if (game->status == STATUS_WIN_A || game->status == STATUS_WIN_B) {
            if (now - session->end_game_timer >= server_timeout) {
                free(session->game);
                session->game = NULL;
                session->is_taken = false;
                free_indices[free_count++] = i; // Return index to the pool
            }
            continue;
        }

        // Condition 2: Clean up if Player A disconnected while waiting
        if (game->status == STATUS_WAITING_FOR_OPPONENT) {
            if (now - session->last_msg_a >= server_timeout) {
                free(session->game);
                session->game = NULL;
                session->is_taken = false;
                free_indices[free_count++] = i;
            }
            continue;
        }

        // Condition 3: Check for timeouts during an active game
        bool a_timeout = (now - session->last_msg_a >= server_timeout);
        bool b_timeout = (now - session->last_msg_b >= server_timeout);

        if (a_timeout || b_timeout) {
            if (a_timeout && !b_timeout) game->status = STATUS_WIN_B;
            else if (b_timeout && !a_timeout) game->status = STATUS_WIN_A;
            else game->status = STATUS_WIN_A; // Default winner if both timeout

            session->end_game_timer = now;
        }
    }
}

// --- SETUP & PARSING UTILITIES ---

// Parses command-line arguments using getopt
static void parse_arguments(int argc, char *argv[], char **pawn_row,
                            char **address, uint16_t *port, uint8_t *timeout) {
    int opt;
    bool has_r = false, has_a = false, has_p = false, has_t = false;

    while ((opt = getopt(argc, argv, "r:a:p:t:")) != -1) {
        switch (opt) {
            case 'r':
                *pawn_row = optarg;
                has_r = true;
                break;
            case 'a':
                *address = optarg;
                has_a = true;
                break;
            case 'p':
                *port = read_port(optarg);
                has_p = true;
                break;
            case 't': {
                // Safely parse timeout to prevent undefined behavior
                char *endptr;
                errno = 0;
                long val = strtol(optarg, &endptr, 10);
                if (errno != 0 || *endptr != '\0' || optarg == endptr ||
                    val < 1 || val > 99) {
                    fatal("Server timeout must be an integer between 1 and 99");
                }
                *timeout = (uint8_t)val;
                has_t = true;
                break;
            }
            default:
                usage(argv[0]);
        }
    }

    if (!has_r || !has_a || !has_p || !has_t) usage(argv[0]);
}

// Validates the initial board and initializes the object pool
static void init_server_state(const char *pawn_row, uint8_t timeout) {
    server_timeout = timeout;

    size_t row_len = strlen(pawn_row);
    if (row_len < 1 || row_len > 256 || pawn_row[0] != '1' ||
        pawn_row[row_len - 1] != '1') {
        fatal("Invalid pawn_row. Must start and end with '1'.");
    }
    for (size_t i = 0; i < row_len; ++i) {
        if (pawn_row[i] != '0' && pawn_row[i] != '1') {
            fatal("pawn_row must contain only 0 or 1");
        }
    }

    // Create a binary template for the board to clone later
    template_max_pawn = row_len - 1;
    template_row_size = (template_max_pawn / 8) + 1;
    template_pawn_row = create_pawn_row(pawn_row);

    // Populate the stack of free indices
    memset(game_table, 0, sizeof(game_table));
    for (int i = 0; i < MAX_ACTIVE_GAMES; ++i) {
        free_indices[i] = i;
    }
}

// --- MESSAGE ROUTING ---

// Processes MSG_JOIN specifically
static void handle_join_msg(int socket_fd, uint8_t *buffer,
                            ssize_t received_length,
                            struct sockaddr_in *client_address,
                            socklen_t address_length) {
    client_msg_join_t *msg = (client_msg_join_t *)buffer;
    uint32_t pid = ntohl(msg->player_id);

    // Player ID cannot be zero according to the spec
    if (pid == 0) {
        send_wrong_msg(socket_fd, buffer, received_length, 1,
                       client_address, address_length);
        return;
    }

    game_session_t *session = handle_client_join(pid, client_address,
                                                 address_length);
    if (session) {
        send_game_state(socket_fd, session, client_address, address_length);
    }
}

// Processes gameplay messages (MOVE, KEEP_ALIVE, GIVE_UP)
static void handle_gameplay_msg(int socket_fd, uint8_t *buffer,
                                ssize_t received_length, uint8_t msg_type,
                                struct sockaddr_in *client_address,
                                socklen_t address_length) {
    uint32_t pid = 0, gid = 0;
    uint8_t pawn = 0;

    // Unpack data based on message size/type
    if (msg_type == MSG_MOVE_1 || msg_type == MSG_MOVE_2) {
        client_msg_move_t *msg = (client_msg_move_t *)buffer;
        pid = ntohl(msg->player_id);
        gid = ntohl(msg->game_id);
        pawn = msg->pawn;
    } else {
        client_msg_basic_t *msg = (client_msg_basic_t *)buffer;
        pid = ntohl(msg->player_id);
        gid = ntohl(msg->game_id);
    }

    if (pid == 0) {
        send_wrong_msg(socket_fd, buffer, received_length, 1,
                       client_address, address_length);
        return;
    }

    game_session_t *session = find_game_session(gid);
    if (!session) {
        send_wrong_msg(socket_fd, buffer, received_length, 5, // game_id err
                       client_address, address_length);
        return;
    }

    // Verify if the player belongs to this game
    bool is_a = (session->game->player_a_id == pid);
    bool is_b = (session->game->player_b_id == pid);
    if (!is_a && !is_b) {
        send_wrong_msg(socket_fd, buffer, received_length, 1,
                       client_address, address_length);
        return;
    }

    // Update activity timers to prevent timeouts
    time_t now = time(NULL);
    if (is_a) {
        session->last_msg_a = now;
        session->client_addr_a = *client_address;
        session->addr_len_a = address_length;
    }
    if (is_b) {
        session->last_msg_b = now;
        session->client_addr_b = *client_address;
        session->addr_len_b = address_length;
    }

    // Prolong retention timer if the game is already finished
    if (session->game->status == STATUS_WIN_A ||
        session->game->status == STATUS_WIN_B) {
        session->end_game_timer = now;
    }

    // Apply game rules based on message type
    if (msg_type == MSG_GIVE_UP) {
        bool is_turn = (is_a && session->game->status == STATUS_TURN_A) ||
                       (is_b && session->game->status == STATUS_TURN_B);

        // A player can only resign during their own turn
        if (is_turn && session->game->status != STATUS_WIN_A &&
            session->game->status != STATUS_WIN_B) {
            session->game->status = is_a ? STATUS_WIN_B : STATUS_WIN_A;
            session->end_game_timer = now;
        }
    } else if (msg_type == MSG_MOVE_1 || msg_type == MSG_MOVE_2) {
        uint8_t knock_count = (msg_type == MSG_MOVE_1) ? 1 : 2;
        bool is_turn = (is_a && session->game->status == STATUS_TURN_A) ||
                       (is_b && session->game->status == STATUS_TURN_B);

        // Apply move only if it's strictly legal
        if (is_turn && is_valid_move(session->game, pawn, knock_count)) {
            apply_move(session->game, pawn, knock_count);

            if (is_game_over(session->game)) {
                session->game->status = is_a ? STATUS_WIN_A : STATUS_WIN_B;
                session->end_game_timer = now;
            } else {
                session->game->status = is_a ? STATUS_TURN_B : STATUS_TURN_A;
            }
        }
    }

    // Always reply with the updated (or unchanged) state
    send_game_state(socket_fd, session, client_address, address_length);
}

// Validates packet length and routes the message
static void process_client_message(int socket_fd, uint8_t *buffer,
                                   ssize_t received_length,
                                   struct sockaddr_in *client_address,
                                   socklen_t address_length) {
    uint8_t msg_type = buffer[0];
    ssize_t expected_len = 0;

    // Determine expected length for strict validation
    switch (msg_type) {
        case MSG_JOIN:       expected_len = 5; break;
        case MSG_MOVE_1:
        case MSG_MOVE_2:     expected_len = 10; break;
        case MSG_KEEP_ALIVE:
        case MSG_GIVE_UP:    expected_len = 9; break;
        default:             expected_len = 0; break;
    }

    if (expected_len == 0 || received_length != expected_len) {
        uint8_t err_idx = 0;
        if (expected_len == 0) {
            err_idx = 0; // Unknown msg type
        } else if (received_length < expected_len) {
            err_idx = (uint8_t)received_length; // Truncated packet
        } else {
            err_idx = (uint8_t)expected_len; // Appended garbage
        }

        send_wrong_msg(socket_fd, buffer, received_length, err_idx,
                       client_address, address_length);
        return;
    }

    // Route to specific handlers
    if (msg_type == MSG_JOIN) {
        handle_join_msg(socket_fd, buffer, received_length,
                        client_address, address_length);
    } else {
        handle_gameplay_msg(socket_fd, buffer, received_length, msg_type,
                            client_address, address_length);
    }
}

// --- MAIN FUNCTION ---

int main(int argc, char *argv[]) {
    char *pawn_row = NULL;
    char *address = NULL;
    uint16_t port = 0;
    uint8_t timeout = 0;

    // 1. Initialization
    parse_arguments(argc, argv, &pawn_row, &address, &port, &timeout);
    init_server_state(pawn_row, timeout);

    // 2. Socket setup
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) syserr("cannot create a socket");

    struct sockaddr_in server_address = get_server_address(address, port);
    if (bind(socket_fd, (struct sockaddr *)&server_address,
             (socklen_t)sizeof(server_address)) < 0) {
        syserr("bind");
    }

    // Timeout allows the blocking recvfrom to return periodically for GC
    struct timeval to = {.tv_sec = 0, .tv_usec = SOCK_TIMEOUT};
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to)) < 0) {
        fatal("setsockopt SO_RCVTIMEO failed");
    }

    printf("Server listening on %s:%d with timeout %ds...\n",
           address, port, timeout);

    // 3. Main Event Loop
    while (true) {
        static uint8_t buffer[MAX_CLIENT_MSG_SIZE];
        memset(buffer, 0, sizeof(buffer));

        struct sockaddr_in client_address;
        socklen_t address_length = (socklen_t)sizeof(client_address);

        ssize_t received_length = recvfrom(socket_fd, buffer,
                                           MAX_CLIENT_MSG_SIZE, 0,
                                           (struct sockaddr *)&client_address,
                                           &address_length);

        // Run Garbage Collector on every loop iteration
        check_and_cleanup_expired_games();

        // Handle timeout gracefully (no packets received)
        if (received_length < 0) continue;

        process_client_message(socket_fd, buffer, received_length,
                               &client_address, address_length);
    }

    close(socket_fd);
    return 0;
}