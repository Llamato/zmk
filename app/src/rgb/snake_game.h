// Pure snake game logic for the rgb snake effect.
//
// Deliberately free of Zephyr and of any LED knowledge: the caller supplies the
// board geometry and which cells actually have a LED under them, so the exact
// same code that runs on the keyboard is exercised by tests/snake_sim.c on a
// host. Everything here works on matrix cells, never on led indexes.
//
// Rules implemented here:
//   - the snake lives on the matrix grid, one cell per body segment
//   - cells without a LED under them (iso stagger, the mode row) are gaps: the
//     head slides straight over them and lands on the next lit cell
//   - walls are either deadly (wrap = false) or teleport to the other side
//   - eating grows the snake by one segment and scores one point
//   - running into the body is fatal, following your own tail is legal
//   - a turn straight back into the neck is ignored instead of being fatal

#ifndef ZMK_SNAKE_GAME_H
#define ZMK_SNAKE_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SNAKE_GAME_MAX_CELLS 255 // uint8_t cell ids
#define SNAKE_GAME_NO_CELL 0xFF  // not a cell / cell is not part of the snake
#define SNAKE_GAME_START_LEN 3

#define SNAKE_GAME_DIR_UP 0
#define SNAKE_GAME_DIR_DOWN 1
#define SNAKE_GAME_DIR_LEFT 2
#define SNAKE_GAME_DIR_RIGHT 3

#define SNAKE_GAME_MAX_CATCHUP_MS 200 // never fast-forward more than this
#define SNAKE_GAME_OVER_MS 2500       // corpse fade, then respawn

enum snake_game_death {
    SNAKE_DEATH_NONE = 0,
    SNAKE_DEATH_SELF,   // ran into the body
    SNAKE_DEATH_WALL,   // ran off a board that does not wrap
    SNAKE_DEATH_NO_CELL // no lit cell left in that direction
};

typedef struct {
    uint8_t rows;
    uint8_t cols;
    uint8_t cells;      // rows * cols
    uint8_t lit_count;  // cells that have a LED under them
    bool wrap;

    uint8_t body[SNAKE_GAME_MAX_CELLS];     // [0] is the head
    uint8_t cell_pos[SNAKE_GAME_MAX_CELLS]; // cell -> index in body, NO_CELL if free
    bool lit[SNAKE_GAME_MAX_CELLS];         // cell has a LED under it

    uint8_t len;
    uint8_t start_len;
    uint8_t dir;
    uint8_t food; // NO_CELL when the board has nothing left to eat
    uint8_t score;
    bool alive;
    uint8_t death;

    uint8_t want_dir;
    bool want_dir_valid;
    bool want_restart;

    uint16_t step_ms;      // time between two moves
    uint16_t game_over_ms; // how long the corpse is shown
    uint32_t rng;
    uint32_t dead_ms; // clock value at death
    uint32_t last_ms; // clock value of the previous tick
    uint32_t acc;     // fractional step accumulator, in 1/256 ms
} snake_game_t;

static inline int8_t snake_game_dr(uint8_t dir) {
    static const int8_t dr[4] = {-1, 1, 0, 0};
    return dir <= SNAKE_GAME_DIR_RIGHT ? dr[dir] : 0;
}

static inline int8_t snake_game_dc(uint8_t dir) {
    static const int8_t dc[4] = {0, 0, -1, 1};
    return dir <= SNAKE_GAME_DIR_RIGHT ? dc[dir] : 0;
}

static inline uint8_t snake_game_cell(const snake_game_t *g, uint8_t row, uint8_t col) {
    return (uint8_t)(row * g->cols + col);
}

static inline uint8_t snake_game_cell_row(const snake_game_t *g, uint8_t cell) {
    return (uint8_t)(cell / g->cols);
}

static inline uint8_t snake_game_cell_col(const snake_game_t *g, uint8_t cell) {
    return (uint8_t)(cell % g->cols);
}

static inline uint8_t snake_game_pos_of(const snake_game_t *g, uint8_t cell) {
    return cell < g->cells ? g->cell_pos[cell] : SNAKE_GAME_NO_CELL;
}

static inline bool snake_game_is_food(const snake_game_t *g, uint8_t cell) {
    return g->alive && cell == g->food;
}

static inline uint32_t snake_game_dead_for(const snake_game_t *g, uint32_t now) {
    return g->alive ? 0 : now - g->dead_ms;
}

// xorshift32, reseeded per game so the food is not in the same place every round
static inline uint32_t snake_game_rand(snake_game_t *g) {
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

static inline uint8_t snake_game_rand_below(snake_game_t *g, uint8_t bound) {
    // the high bits of xorshift32 are the better behaved ones
    return bound == 0 ? 0 : (uint8_t)((snake_game_rand(g) >> 16) % bound);
}

static inline void snake_game_pos_map_update(snake_game_t *g) {
    memset(g->cell_pos, SNAKE_GAME_NO_CELL, sizeof(g->cell_pos));
    for (uint8_t i = 0; i < g->len; i++) {
        g->cell_pos[g->body[i]] = i;
    }
}

static inline void snake_game_die(snake_game_t *g, uint8_t reason, uint32_t now) {
    g->alive = false;
    g->death = reason;
    g->dead_ms = now;
    g->want_dir_valid = false;
    g->want_restart = false;
}

// food always lands on a lit cell that the snake does not occupy
static inline void snake_game_spawn_food(snake_game_t *g) {
    for (uint8_t attempt = 0; attempt < 64; attempt++) {
        uint8_t cell = snake_game_rand_below(g, g->cells);
        if (g->lit[cell] && g->cell_pos[cell] == SNAKE_GAME_NO_CELL) {
            g->food = cell;
            return;
        }
    }

    for (uint8_t cell = 0; cell < g->cells; cell++) {
        if (g->lit[cell] && g->cell_pos[cell] == SNAKE_GAME_NO_CELL) {
            g->food = cell;
            return;
        }
    }

    g->food = SNAKE_GAME_NO_CELL; // nothing left to eat
}

// spawn in the middle-left of the board, on a lit cell, heading right, with the
// body stretched back over lit cells only
static inline void snake_game_reset(snake_game_t *g, uint32_t now) {
    uint8_t ideal_row = g->rows / 2;
    uint8_t ideal_col = g->cols / 3;
    uint8_t start = SNAKE_GAME_NO_CELL;
    int best = 1 << 30;

    for (uint8_t cell = 0; cell < g->cells; cell++) {
        if (!g->lit[cell]) {
            continue;
        }

        int row = snake_game_cell_row(g, cell);
        int col = snake_game_cell_col(g, cell);
        int dist = (row > ideal_row ? row - ideal_row : ideal_row - row) +
                   (col > ideal_col ? col - ideal_col : ideal_col - col);
        if (dist < best) {
            best = dist;
            start = cell;
        }
    }

    if (start == SNAKE_GAME_NO_CELL) { // a board with no lit keys cannot be played
        g->len = 0;
        g->food = SNAKE_GAME_NO_CELL;
        snake_game_pos_map_update(g);
        snake_game_die(g, SNAKE_DEATH_NO_CELL, now);
        return;
    }

    g->len = 1;
    g->body[0] = start;

    for (uint8_t i = 1; i < SNAKE_GAME_START_LEN; i++) {
        uint8_t row = snake_game_cell_row(g, start);
        uint8_t col = snake_game_cell_col(g, start);
        uint8_t back = SNAKE_GAME_NO_CELL;

        for (uint8_t guard = 0; guard < g->cells; guard++) {
            int16_t ncol = (int16_t)col - 1;
            if (ncol < 0) {
                if (!g->wrap) {
                    break;
                }
                ncol = g->cols - 1;
            }

            uint8_t cell = snake_game_cell(g, row, (uint8_t)ncol);
            if (g->lit[cell]) {
                back = cell;
                break;
            }

            col = (uint8_t)ncol;
        }

        if (back == SNAKE_GAME_NO_CELL) {
            break;
        }

        g->body[g->len++] = back;
        start = back;
    }

    g->start_len = g->len;
    g->dir = SNAKE_GAME_DIR_RIGHT;
    g->score = 0;
    g->alive = true;
    g->death = SNAKE_DEATH_NONE;
    g->want_dir_valid = false;
    g->want_restart = false;
    g->acc = 0;
    g->last_ms = now;

    if (g->rng == 0) {
        g->rng = 0x9E3779B9u;
    }

    snake_game_pos_map_update(g);
    snake_game_spawn_food(g);
}

static inline void snake_game_init(snake_game_t *g, uint8_t rows, uint8_t cols, bool wrap) {
    memset(g, 0, sizeof(*g));

    g->rows = rows;
    g->cols = cols;
    g->cells = (uint8_t)(rows * cols);
    g->wrap = wrap;
    g->step_ms = 1000;
    g->game_over_ms = SNAKE_GAME_OVER_MS;
    g->rng = 0x1234ABCDu ^ ((uint32_t)rows << 8) ^ cols;

    for (uint8_t cell = 0; cell < g->cells; cell++) {
        g->lit[cell] = true;
    }

    g->lit_count = g->cells;
}

static inline void snake_game_set_lit(snake_game_t *g, uint8_t row, uint8_t col, bool lit) {
    if (row >= g->rows || col >= g->cols) {
        return;
    }

    uint8_t cell = snake_game_cell(g, row, col);
    if (g->lit[cell] != lit) {
        g->lit[cell] = lit;
        g->lit_count = (uint8_t)(g->lit_count + (lit ? 1 : -1));
    }
}

// called from the kscan work queue, the rgb thread consumes it in snake_game_step
static inline void snake_game_turn(snake_game_t *g, uint8_t dir) {
    if (dir > SNAKE_GAME_DIR_RIGHT) {
        return;
    }

    g->want_dir = dir;
    g->want_dir_valid = true;
    if (!g->alive) {
        g->want_restart = true;
    }
}

static inline bool snake_game_is_reverse(const snake_game_t *g, uint8_t turn) {
    return snake_game_dr(turn) == -snake_game_dr(g->dir) &&
           snake_game_dc(turn) == -snake_game_dc(g->dir);
}

// one move of the head. returns true when the snake is still alive afterwards
static inline bool snake_game_step(snake_game_t *g, uint32_t now) {
    // a restart request is honoured even while the corpse is being shown
    if (g->want_restart) {
        g->want_restart = false;
        snake_game_reset(g, now);
        return true;
    }

    if (!g->alive) {
        return false;
    }

    // steering straight back into the neck would be an instant death, so it is ignored
    if (g->want_dir_valid) {
        if (!snake_game_is_reverse(g, g->want_dir)) {
            g->dir = g->want_dir;
        }
        g->want_dir_valid = false;
    }

    int8_t dr = snake_game_dr(g->dir);
    int8_t dc = snake_game_dc(g->dir);
    int16_t row = snake_game_cell_row(g, g->body[0]);
    int16_t col = snake_game_cell_col(g, g->body[0]);
    uint8_t next = SNAKE_GAME_NO_CELL;

    // dark cells are gaps in the board, the head keeps going until it reaches a lit one
    for (uint8_t guard = 0; guard < g->cells; guard++) {
        row += dr;
        col += dc;

        if (row < 0 || row >= g->rows || col < 0 || col >= g->cols) {
            if (!g->wrap) {
                snake_game_die(g, SNAKE_DEATH_WALL, now);
                return false;
            }

            row = (row + g->rows) % g->rows;
            col = (col + g->cols) % g->cols;
        }

        uint8_t cell = snake_game_cell(g, (uint8_t)row, (uint8_t)col);
        if (g->lit[cell]) {
            next = cell;
            break;
        }
    }

    if (next == SNAKE_GAME_NO_CELL) {
        snake_game_die(g, SNAKE_DEATH_NO_CELL, now);
        return false;
    }

    bool eats = next == g->food;

    // the tail cell is vacated by this move unless the snake grows
    uint8_t solid = eats ? g->len : (uint8_t)(g->len - 1);
    for (uint8_t i = 0; i < solid; i++) {
        if (g->body[i] == next) {
            snake_game_die(g, SNAKE_DEATH_SELF, now);
            return false;
        }
    }

    if (eats) {
        if (g->len >= SNAKE_GAME_MAX_CELLS) { // the whole board is snake
            snake_game_die(g, SNAKE_DEATH_SELF, now);
            return false;
        }

        g->len++;
        g->score++;
    }

    for (uint8_t i = g->len - 1; i > 0; i--) {
        g->body[i] = g->body[i - 1];
    }
    g->body[0] = next;

    snake_game_pos_map_update(g);

    if (eats) {
        snake_game_spawn_food(g);
    }

    return true;
}

// advance the game to "now"; the rgb task renders one frame over several
// iterations, so the caller only ticks once per frame
static inline void snake_game_tick(snake_game_t *g, uint32_t now) {
    uint32_t elapsed = now > g->last_ms ? now - g->last_ms : 0;
    if (elapsed > SNAKE_GAME_MAX_CATCHUP_MS) {
        elapsed = SNAKE_GAME_MAX_CATCHUP_MS;
    }

    g->last_ms = now;
    g->acc += elapsed * 256;

    // an arrow key restarts right away, otherwise the game respawns on its own
    // once the corpse has faded out
    if (!g->alive && (g->want_restart || now - g->dead_ms >= g->game_over_ms)) {
        snake_game_reset(g, now);
    }

    uint32_t step = (uint32_t)g->step_ms * 256;
    while (g->alive && g->acc >= step) {
        g->acc -= step;
        snake_game_step(g, now);
    }
}

#endif // ZMK_SNAKE_GAME_H
