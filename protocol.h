#ifndef KAYLES_PROTOCOL_H
#define KAYLES_PROTOCOL_H

#include <stdint.h>

// ==========================================
//    DICTIONARIES (ENUMS)
// ==========================================

// Current stage of the game (8-bit)
typedef enum {
    STATUS_WAITING_FOR_OPPONENT = 0, // Player A is connected, waiting for Player B
    STATUS_TURN_A = 1,               // Waiting for Player A's move
    STATUS_TURN_B = 2,               // Waiting for Player B's move
    STATUS_WIN_A  = 3,               // Player A has won
    STATUS_WIN_B  = 4,               // Player B has won
    STATUS_WRONG_MSG = 255           // Special status for MSG_WRONG_MSG
} game_status_t;

// Type of message sent from the client to the server (8-bit)
typedef enum {
    MSG_JOIN       = 0, // Request to join a game
    MSG_MOVE_1     = 1, // Move: knock down one pawn
    MSG_MOVE_2     = 2, // Move: knock down two adjacent pawns
    MSG_KEEP_ALIVE = 3, // Ping: confirm client is still active
    MSG_GIVE_UP    = 4  // Resign: give up the current game
} msg_type_t;


// ==========================================
//    SERVER -> CLIENT (Server responses)
// ==========================================

// Standard game state response (MSG_GAME_STATE)
typedef struct __attribute__((__packed__)) {
    uint32_t game_id;     // Unique game identifier
    uint32_t player_a_id; // Player A identifier (must be non-zero)
    uint32_t player_b_id; // Player B identifier (0 means no player yet)
    uint8_t  status;      // Current game stage (game_status_t)
    uint8_t  max_pawn;    // Maximum pawn number (index of the last pawn)

    // Bitmap representing the row of pawns.
    // 1 = pawn is standing, 0 = pawn is knocked down or empty space.
    // Pawn 0 is stored in the most significant bit (MSB) of the first byte.
    // Size: floor(max_pawn / 8) + 1.
    // This is a Flexible Array Member and must be the last field.
    uint8_t  pawn_row[];
} server_game_state_t;

// Error response (MSG_WRONG_MSG)
typedef struct __attribute__((__packed__)) {
    uint8_t original_msg[12]; // Up to first 12 bytes of the client's bad message (padded with 0s)
    uint8_t status;           // Always 255 (STATUS_WRONG_MSG). At byte offset 12!
    uint8_t error_index;      // Index of the byte the server couldn't interpret
} server_msg_wrong_t;


// ==========================================
//    CLIENT -> SERVER (Client Messages)
// ==========================================

// MSG_JOIN (Total length: 5 bytes)
// Sent when a player wants to join the matchmaking queue.
typedef struct __attribute__((__packed__)) {
    uint8_t  msg_type;   // Expected value: 0 (MSG_JOIN)
    uint32_t player_id;  // Unique player identifier
} client_msg_join_t;

// MSG_MOVE_1 and MSG_MOVE_2 (Total length: 10 bytes)
// Sent when a player makes a move.
// If msg_type == 2, it knocks down 'pawn' and 'pawn + 1'.
typedef struct __attribute__((__packed__)) {
    uint8_t  msg_type;   // Expected value: 1 (MSG_MOVE_1) or 2 (MSG_MOVE_2)
    uint32_t player_id;  // Unique player identifier
    uint32_t game_id;    // Target game identifier
    uint8_t  pawn;       // The pawn number to knock down
} client_msg_move_t;

// MSG_KEEP_ALIVE and MSG_GIVE_UP (Total length: 9 bytes)
// Sent to ping the server or forfeit the game.
typedef struct __attribute__((__packed__)) {
    uint8_t  msg_type;   // Expected value: 3 (MSG_KEEP_ALIVE) or 4 (MSG_GIVE_UP)
    uint32_t player_id;  // Unique player identifier
    uint32_t game_id;    // Target game identifier
} client_msg_basic_t;

#endif //KAYLES_PROTOCOL_H