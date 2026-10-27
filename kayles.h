#ifndef KAYLES_KAYLES_H
#define KAYLES_KAYLES_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// Allocates and initializes the bitmap array for the pawns.
// The caller is responsible for freeing the allocated memory.
uint8_t *create_pawn_row(const char *pawn_row_str);

// Checks if the pawn at the given index is currently standing (bit == 1).
bool is_pawn_standing(const uint8_t *pawn_row, uint8_t pawn_idx);

// Knocks down the pawn at the given index (sets its bit to 0).
void knock_down_pawn(uint8_t *pawn_row, uint8_t pawn_idx);

// Applies a move by knocking down 1 or 2 pawns starting at pawn_idx.
void apply_move(server_game_state_t *game, uint8_t pawn_idx, uint8_t count);

// Validates if a move is legal (pawns are standing and within bounds).
bool is_valid_move(const server_game_state_t *game, uint8_t pawn_idx,
                   uint8_t count);

// Checks if all pawns are knocked down (the bitmap is entirely 0).
bool is_game_over(const server_game_state_t *game);

#endif //KAYLES_KAYLES_H