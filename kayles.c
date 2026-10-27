#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "kayles.h"
#include "protocol.h"
#include "err.h"

uint8_t *create_pawn_row(const char *pawn_row_str) {
    size_t len = strlen(pawn_row_str);
    size_t pawn_row_len = (len - 1) / 8 + 1;

    uint8_t *pawn_row = (uint8_t *)calloc(pawn_row_len, sizeof(uint8_t));
    if (!pawn_row) {
        fatal("calloc failed");
    }

    for (size_t i = 0; i < len; ++i) {
        if (pawn_row_str[i] == '1') {
            size_t byte_idx = i / 8;
            // Pawn 0 is stored in the most significant bit (MSB)
            size_t bit_idx = 7 - (i % 8);
            pawn_row[byte_idx] |= (1 << bit_idx);
        }
    }
    return pawn_row;
}

bool is_pawn_standing(const uint8_t *pawn_row, uint8_t pawn_idx) {
    size_t byte_idx = pawn_idx / 8;
    // Map pawn index to the corresponding bit, starting from MSB
    size_t bit_idx = 7 - (pawn_idx % 8);
    return (pawn_row[byte_idx] & (1 << bit_idx)) != 0;
}

void knock_down_pawn(uint8_t *pawn_row, uint8_t pawn_idx) {
    size_t byte_idx = pawn_idx / 8;
    size_t bit_idx = 7 - (pawn_idx % 8);
    pawn_row[byte_idx] &= ~(1 << bit_idx);
}

void apply_move(server_game_state_t *game, uint8_t pawn_idx, uint8_t count) {
    knock_down_pawn(game->pawn_row, pawn_idx);
    if (count == 2) {
        knock_down_pawn(game->pawn_row, pawn_idx + 1);
    }
}

bool is_valid_move(const server_game_state_t *game, uint8_t pawn_idx,
                   uint8_t count) {
    if (pawn_idx > game->max_pawn) return false;

    // Cast to uint16_t prevents wrap-around if pawn_idx is 255
    if (count == 2 && ((uint16_t)pawn_idx + 1) > game->max_pawn) return false;

    if (!is_pawn_standing(game->pawn_row, pawn_idx)) return false;

    if (count == 2) {
        if (!is_pawn_standing(game->pawn_row, pawn_idx + 1)) return false;
    }

    return true;
}

bool is_game_over(const server_game_state_t *game) {
    size_t byte_len = (game->max_pawn / 8) + 1;

    for (size_t i = 0; i < byte_len; ++i) {
        if (game->pawn_row[i] != 0) {
            return false; // Found at least one standing pawn
        }
    }

    return true;
}