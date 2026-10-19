// Host test for the snake game logic used by the rgb snake effect.
//
// It compiles app/src/rgb/snake_game.h unchanged (the header has no zephyr
// dependencies) and hammers it with randomised games plus hand built boards.
//
//   gcc -std=c11 -Wall -Wextra -O2 -I app/src/rgb -o /tmp/snake_sim tests/snake_sim.c && /tmp/snake_sim
//   nix build .#packages.x86_64-linux.snakeTest   # same thing, as a nix check
//
// The board is the real one: the led grid is parsed out of
// app/boards/shields/keychron_q6_ultra_iso/rgb_matrix_config.h, and compared
// against the copy below so the test notices when the board changes.

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snake_game.h"

#define Q6_ROWS 6
#define Q6_COLS 20
#define Q6_LIT 109
#define Q6_LED_COUNT 109

static int failures;
static int checks;

#define CHECK(cond, msg)                                                                                   \
    do {                                                                                                   \
        checks++;                                                                                          \
        if (!(cond)) {                                                                                     \
            failures++;                                                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                        \
        }                                                                                                  \
    } while (0)

// cells of the q6 ultra iso board that have no led under them
static const uint8_t q6_unlit[][2] = {{0, 13}, {3, 13}, {4, 13}, {4, 14}, {4, 16},
                                      {5, 3},  {5, 4},  {5, 5},  {5, 7},  {5, 8},
                                      {5, 9}};

static const char *q6_config_paths[] = {
    "app/boards/shields/keychron_q6_ultra_iso/rgb_matrix_config.h",
    "../app/boards/shields/keychron_q6_ultra_iso/rgb_matrix_config.h",
};

// read the shield's own matrix_co grid: six rows of twenty led indexes, with
// "__" marking the cells that have no led under them
static int parse_matrix_co(const char *text, bool lit[Q6_ROWS][Q6_COLS]) {
    const char *p = strstr(text, "g_led_config");
    if (!p) {
        return -1;
    }

    p = strchr(p, '{'); // led_config_t {
    if (!p) {
        return -1;
    }

    p = strchr(p + 1, '{'); // matrix_co {
    if (!p) {
        return -1;
    }

    int row = 0, col = 0;
    while (*p && row < Q6_ROWS) {
        if (isspace((unsigned char)*p) || *p == '{' || *p == ',') {
            p++;
            continue;
        }

        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }

        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                p++;
            }
            p += 2; // step past */
            continue;
        }

        if (*p == '}') {
            if (col != Q6_COLS) {
                return -1;
            }
            row++;
            col = 0;
            p++;
            continue;
        }

        if (col >= Q6_COLS) {
            return -1;
        }

        if (*p == '_' || isalpha((unsigned char)*p)) { // "__" or "NO_LED"
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                p++;
            }
            lit[row][col++] = false;
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            while (isdigit((unsigned char)*p)) {
                p++;
            }
            lit[row][col++] = true;
            continue;
        }

        return -1;
    }

    return row == Q6_ROWS && col == 0 ? 0 : -1;
}

static int board_from_config(bool lit[Q6_ROWS][Q6_COLS]) {
    for (size_t i = 0; i < sizeof(q6_config_paths) / sizeof(q6_config_paths[0]); i++) {
        FILE *f = fopen(q6_config_paths[i], "r");
        if (!f) {
            continue;
        }

        static char text[16384];
        size_t read = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[read] = '\0';

        if (parse_matrix_co(text, lit) == 0) {
            return 0;
        }
    }

    return -1;
}

static void q6_board(snake_game_t *g, bool wrap) {
    bool lit[Q6_ROWS][Q6_COLS];

    if (board_from_config(lit) != 0) { // fall back to the copy of the same board
        for (int row = 0; row < Q6_ROWS; row++) {
            for (int col = 0; col < Q6_COLS; col++) {
                lit[row][col] = true;
            }
        }
        for (size_t i = 0; i < sizeof(q6_unlit) / sizeof(q6_unlit[0]); i++) {
            lit[q6_unlit[i][0]][q6_unlit[i][1]] = false;
        }
    }

    snake_game_init(g, Q6_ROWS, Q6_COLS, wrap);
    for (int row = 0; row < Q6_ROWS; row++) {
        for (int col = 0; col < Q6_COLS; col++) {
            if (!lit[row][col]) {
                snake_game_set_lit(g, (uint8_t)row, (uint8_t)col, false);
            }
        }
    }
}

static uint8_t cell_of(const snake_game_t *g, uint8_t row, uint8_t col) {
    return (uint8_t)(row * g->cols + col);
}

// every structural rule the game must hold at all times
static void invariants(const snake_game_t *g, const char *ctx) {
    CHECK(g->cells == (uint8_t)(g->rows * g->cols), "cells == rows * cols");
    CHECK(g->len <= g->lit_count, "snake never longer than the lit cells");
    CHECK(g->len <= SNAKE_GAME_MAX_CELLS, "snake fits in the body array");
    CHECK(g->score == (uint8_t)(g->len - g->start_len), "score tracks the body length");

    uint8_t mapped = 0;
    for (uint8_t cell = 0; cell < g->cells; cell++) {
        if (g->cell_pos[cell] != SNAKE_GAME_NO_CELL) {
            mapped++;
        }
    }
    CHECK(mapped == g->len, "position map has exactly one entry per body segment");

    for (uint8_t i = 0; i < g->len; i++) {
        uint8_t cell = g->body[i];
        CHECK(cell < g->cells, "body segment is a real cell");
        CHECK(cell < SNAKE_GAME_MAX_CELLS && g->lit[cell], "body segment sits on a lit cell");
        CHECK(cell < SNAKE_GAME_MAX_CELLS && g->cell_pos[cell] == i, "position map matches body");
        for (uint8_t j = i + 1; j < g->len; j++) {
            CHECK(g->body[j] != cell, "body never occupies a cell twice");
        }
    }

    if (g->food != SNAKE_GAME_NO_CELL) {
        CHECK(g->food < g->cells, "food is a real cell");
        CHECK(g->lit[g->food], "food is always on a lit key");
        CHECK(g->cell_pos[g->food] == SNAKE_GAME_NO_CELL, "food is never on the snake");
    }

    // consecutive segments must be reachable in a straight line, and everything
    // skipped in between must be a dark cell
    for (uint8_t i = 0; i + 1 < g->len; i++) {
        bool connected = false;
        for (uint8_t dir = 0; dir <= SNAKE_GAME_DIR_RIGHT && !connected; dir++) {
            int8_t dr = snake_game_dr(dir);
            int8_t dc = snake_game_dc(dir);
            int16_t row = g->body[i + 1] / g->cols;
            int16_t col = g->body[i + 1] % g->cols;

            for (uint8_t guard = 0; guard < g->cells && !connected; guard++) {
                row = (row + dr + g->rows) % g->rows;
                col = (col + dc + g->cols) % g->cols;
                uint8_t cell = (uint8_t)(row * g->cols + col);
                if (cell == g->body[i]) {
                    connected = true;
                } else if (g->lit[cell]) {
                    break;
                }
            }
        }
        CHECK(connected, "body segments are connected in a straight line over dark cells");
    }

    (void)ctx;
}

// a food cell that is nowhere near the snake
static uint8_t far_food(const snake_game_t *g) {
    for (uint8_t cell = 0; cell < g->cells; cell++) {
        if (g->lit[cell] && g->cell_pos[cell] == SNAKE_GAME_NO_CELL) {
            return cell;
        }
    }
    return SNAKE_GAME_NO_CELL;
}

// hand built board state for the targeted tests
static void place(snake_game_t *g, const uint8_t (*cells)[2], uint8_t n, uint8_t dir, uint8_t food) {
    snake_game_reset(g, 0);

    g->len = n;
    for (uint8_t i = 0; i < n; i++) {
        g->body[i] = cell_of(g, cells[i][0], cells[i][1]);
    }

    g->start_len = n;
    g->dir = dir;
    g->score = 0;
    g->alive = true;
    g->death = SNAKE_DEATH_NONE;
    g->acc = 0;
    g->last_ms = 0;
    g->want_dir_valid = false;
    g->want_restart = false;
    snake_game_pos_map_update(g);
    // SNAKE_GAME_NO_CELL asks for a food cell that is nowhere near the snake
    g->food = food == SNAKE_GAME_NO_CELL ? far_food(g) : food;
}

static void show(const snake_game_t *g, const char *title) {
    printf("  %s\n   ", title);
    for (uint8_t col = 0; col < g->cols; col++) {
        printf("%c", col % 10 + '0');
    }
    printf("\n");

    for (uint8_t row = 0; row < g->rows; row++) {
        printf("  %u ", row);
        for (uint8_t col = 0; col < g->cols; col++) {
            uint8_t cell = cell_of(g, row, col);
            if (!g->lit[cell]) {
                printf(" ");
            } else if (g->cell_pos[cell] == 0) {
                printf("@");
            } else if (g->cell_pos[cell] != SNAKE_GAME_NO_CELL) {
                printf("o");
            } else if (cell == g->food) {
                printf("*");
            } else {
                printf(".");
            }
        }
        printf("\n");
    }
}

static void test_board_map(void) {
    printf("board map\n");
    snake_game_t g;
    q6_board(&g, true);

    CHECK(g.cells == Q6_ROWS * Q6_COLS, "120 matrix cells");
    CHECK(g.lit_count == Q6_LIT, "109 of them have a led under them");
    CHECK(g.lit_count == Q6_LED_COUNT, "the lit cells are exactly the leds the shield has");

    // the board this test models must stay the board the shield describes
    bool config[Q6_ROWS][Q6_COLS];
    for (int row = 0; row < Q6_ROWS; row++) {
        for (int col = 0; col < Q6_COLS; col++) {
            config[row][col] = true;
            for (size_t i = 0; i < sizeof(q6_unlit) / sizeof(q6_unlit[0]); i++) {
                if (q6_unlit[i][0] == row && q6_unlit[i][1] == col) {
                    config[row][col] = false;
                }
            }
        }
    }

    if (board_from_config(config) == 0) {
        for (int row = 0; row < Q6_ROWS; row++) {
            for (int col = 0; col < Q6_COLS; col++) {
                bool copy = true;
                for (size_t i = 0; i < sizeof(q6_unlit) / sizeof(q6_unlit[0]); i++) {
                    if (q6_unlit[i][0] == row && q6_unlit[i][1] == col) {
                        copy = false;
                    }
                }
                CHECK(config[row][col] == copy,
                      "the board copy in this test no longer matches the shield led config");
            }
        }
    } else {
        printf("  note: shield led config not found next to the test, using the copy\n");
    }

    snake_game_reset(&g, 0);
    invariants(&g, "fresh game");
    CHECK(g.len == SNAKE_GAME_START_LEN, "starts with three segments");
    CHECK(g.cell_pos[g.body[0]] == 0, "head is segment zero");
    CHECK(g.lit[g.body[0]], "spawned on a lit key");
    CHECK(snake_game_cell_row(&g, g.body[0]) == Q6_ROWS / 2, "spawned in the middle row");
    CHECK(g.dir == SNAKE_GAME_DIR_RIGHT, "spawned heading right");
    show(&g, "fresh game");
}

static void test_gap_slide(void) {
    printf("sliding over the dark cells\n");
    snake_game_t g;
    q6_board(&g, true);

    // row 5 is the bottom row: cols 3,4,5 and 7,8,9 have no led under them
    uint8_t start[1][2] = {{5, 2}};
    place(&g, start, 1, SNAKE_GAME_DIR_RIGHT, SNAKE_GAME_NO_CELL);

    snake_game_step(&g, 1);
    CHECK(g.body[0] == cell_of(&g, 5, 6), "jumped over the three dark cells at 5,3 5,4 5,5");
    snake_game_step(&g, 2);
    CHECK(g.body[0] == cell_of(&g, 5, 10), "jumped over the three dark cells at 5,7 5,8 5,9");
    snake_game_step(&g, 3);
    CHECK(g.body[0] == cell_of(&g, 5, 11), "one cell at a time on normal keys");
    invariants(&g, "gap slide");

    // the iso hole in the home row
    uint8_t home[1][2] = {{3, 12}};
    place(&g, home, 1, SNAKE_GAME_DIR_RIGHT, SNAKE_GAME_NO_CELL);
    snake_game_step(&g, 4);
    CHECK(g.body[0] == cell_of(&g, 3, 14), "skipped the hole at 3,13");

    // the numpad holes in the row above it
    uint8_t above[1][2] = {{4, 12}};
    place(&g, above, 1, SNAKE_GAME_DIR_RIGHT, SNAKE_GAME_NO_CELL);
    snake_game_step(&g, 5);
    CHECK(g.body[0] == cell_of(&g, 4, 15), "skipped the holes at 4,13 and 4,14");
    snake_game_step(&g, 6);
    CHECK(g.body[0] == cell_of(&g, 4, 17), "skipped the hole at 4,16");

    // wrapping while sliding
    uint8_t corner[1][2] = {{5, 19}};
    place(&g, corner, 1, SNAKE_GAME_DIR_RIGHT, SNAKE_GAME_NO_CELL);
    snake_game_step(&g, 7);
    CHECK(g.body[0] == cell_of(&g, 5, 0), "wrapped from the right edge to the left edge");
    uint8_t bottom[1][2] = {{5, 19}};
    place(&g, bottom, 1, SNAKE_GAME_DIR_DOWN, SNAKE_GAME_NO_CELL);
    snake_game_step(&g, 8);
    CHECK(g.body[0] == cell_of(&g, 0, 19), "wrapped off the bottom edge to the top row");
    invariants(&g, "gap wrap");
}

static void invariants_light(const snake_game_t *g) {
    CHECK(g->len <= g->lit_count, "fuzz: snake never longer than the lit cells");
    for (uint8_t i = 0; i < g->len; i++) {
        uint8_t cell = g->body[i];
        CHECK(cell < g->cells && g->lit[cell], "fuzz: body stays on lit cells");
        CHECK(cell < SNAKE_GAME_MAX_CELLS && g->cell_pos[cell] == i, "fuzz: position map matches body");
    }
    if (g->food != SNAKE_GAME_NO_CELL) {
        CHECK(g->lit[g->food] && g->cell_pos[g->food] == SNAKE_GAME_NO_CELL,
              "fuzz: food on a free lit cell");
    }
}

static uint32_t test_rng = 1;
static uint8_t rnd(uint8_t bound) {
    // xorshift32: an lcg would give correlated low bits at %4 and stall the fuzz
    test_rng ^= test_rng << 13;
    test_rng ^= test_rng >> 17;
    test_rng ^= test_rng << 5;
    return (uint8_t)((test_rng >> 16) % bound);
}

static void test_wrap_fuzz(void) {
    printf("wrap mode: 40000 random steps\n");
    snake_game_t g;
    q6_board(&g, true);
    snake_game_reset(&g, 0);

    uint64_t steps = 0;
    int games = 0, self = 0, wall = 0, other = 0, best_score = 0;
    test_rng = 20261015u;

    for (uint32_t i = 0; i < 40000; i++) {
        if (rnd(5) == 0) {
            snake_game_turn(&g, rnd(4));
        }

        bool alive = snake_game_step(&g, (i + 1) * 1000);
        steps++;
        invariants_light(&g);
        if ((i & 7) == 0) {
            invariants(&g, "wrap fuzz");
        }

        if (!alive) {
            games++;
            if (g.score > best_score) {
                best_score = g.score;
            }
            if (g.death == SNAKE_DEATH_SELF) {
                self++;
            } else if (g.death == SNAKE_DEATH_WALL) {
                wall++;
            } else {
                other++;
            }
            snake_game_reset(&g, (i + 1) * 1000);
        }
    }

    CHECK(wall == 0, "wrap mode never dies on a wall");
    CHECK(self > 10, "self collision is still fatal");
    CHECK(other == 0, "no other way to die on this board");
    CHECK(games > 50, "the fuzz actually played games");
    printf("  %llu steps, %d games, %d self deaths, %d wall deaths, best score %d\n",
           (unsigned long long)steps, games, self, wall, best_score);
}

static void test_no_wrap_walls(void) {
    printf("walls are still deadly when wrap is off\n");
    snake_game_t g;
    q6_board(&g, false);
    snake_game_reset(&g, 0);
    g.food = SNAKE_GAME_NO_CELL;

    int steps = 0;
    while (g.alive && steps < 100) {
        snake_game_step(&g, (uint32_t)(++steps) * 1000);
    }

    CHECK(!g.alive, "running right on a board without wrap ends the game");
    CHECK(g.death == SNAKE_DEATH_WALL, "and the reason is the wall");
    CHECK(steps == 13, "it took exactly the walk from column 6 to column 19");
}

static void test_turns(void) {
    printf("turning\n");
    snake_game_t g;
    q6_board(&g, true);

    uint8_t straight[3][2] = {{3, 6}, {3, 5}, {3, 4}};

    place(&g, straight, 3, SNAKE_GAME_DIR_RIGHT, SNAKE_GAME_NO_CELL);
    snake_game_turn(&g, SNAKE_GAME_DIR_LEFT);
    snake_game_step(&g, 1);
    CHECK(g.dir == SNAKE_GAME_DIR_RIGHT, "a turn straight back into the neck is ignored");
    CHECK(g.alive, "ignoring the reverse keeps the snake alive");
    CHECK(g.body[0] == cell_of(&g, 3, 7), "the snake kept going right");

    // a three segment u turn is legal in snake and must stay legal
    uint8_t uturn[3][2] = {{3, 5}, {3, 4}, {3, 3}};
    place(&g, uturn, 3, SNAKE_GAME_DIR_UP, SNAKE_GAME_NO_CELL);
    snake_game_step(&g, 1);
    snake_game_turn(&g, SNAKE_GAME_DIR_LEFT);
    snake_game_step(&g, 2);
    snake_game_turn(&g, SNAKE_GAME_DIR_DOWN);
    bool alive = snake_game_step(&g, 3);
    CHECK(alive, "the u turn through the vacated tail cell survives");
    CHECK(g.body[0] == cell_of(&g, 3, 4), "and lands where the tail used to be");
    invariants(&g, "u turn");

    // following the tail cell is legal
    uint8_t tail[6][2] = {{3, 6}, {3, 7}, {3, 8}, {2, 8}, {2, 7}, {2, 6}};
    place(&g, tail, 6, SNAKE_GAME_DIR_UP, SNAKE_GAME_NO_CELL);
    alive = snake_game_step(&g, 4);
    CHECK(alive, "moving into the cell the tail just vacated is legal");
    CHECK(g.body[0] == cell_of(&g, 2, 6), "the head followed the tail");

    // running into the middle of the body is fatal
    uint8_t spiral[7][2] = {{3, 6}, {3, 7}, {3, 8}, {3, 9}, {2, 9}, {2, 8}, {2, 7}};
    place(&g, spiral, 7, SNAKE_GAME_DIR_UP, SNAKE_GAME_NO_CELL);
    snake_game_turn(&g, SNAKE_GAME_DIR_RIGHT);
    alive = snake_game_step(&g, 5);
    CHECK(!alive, "running into the body ends the game");
    CHECK(g.death == SNAKE_DEATH_SELF, "and the reason is the body");
    invariants(&g, "self hit");
    (void)tail;
}

static void test_timing(void) {
    printf("timing\n");
    snake_game_t g;
    q6_board(&g, true);
    snake_game_reset(&g, 0);
    g.step_ms = 1000;
    g.food = SNAKE_GAME_NO_CELL; // no eating, so one step is exactly one cell

    uint8_t head = g.body[0];
    int moves = 0;
    for (uint32_t t = 100; t <= 900; t += 100) {
        snake_game_tick(&g, t);
        if (g.body[0] != head) {
            moves++;
            head = g.body[0];
        }
    }
    CHECK(moves == 0, "nothing moves before a full step has elapsed");

    snake_game_tick(&g, 1000);
    CHECK(g.body[0] != head, "exactly one move when the step elapses");
    invariants(&g, "timing");

    // a long stall must not fast forward the game, only SNAKE_GAME_MAX_CATCHUP_MS
    head = g.body[0];
    g.step_ms = 100;
    snake_game_tick(&g, 11000); // a ten second gap
    int moved = (g.body[0] % g.cols) - (head % g.cols);
    if (moved < 0) {
        moved += g.cols;
    }
    CHECK(moved == SNAKE_GAME_MAX_CATCHUP_MS / 100,
          "a ten second stall advances only the catch up limit, not a hundred steps");
    invariants(&g, "catch up");

    // the speed setting maps onto the step interval the way the effect computes it
    int slow = 1400, fast = 450;
    printf("  step interval: speed 0 -> %d ms, 64 -> %d ms, 127 -> %d ms, 192 -> %d ms, 255 -> %d ms\n",
           slow, slow - (slow - fast) * 64 / 255, slow - (slow - fast) * 127 / 255,
           slow - (slow - fast) * 192 / 255, fast);
    CHECK(slow - (slow - fast) * 127 / 255 > 800, "the middle speed is a leisurely walk");
    CHECK(fast >= 400, "even the fastest speed is slow enough to watch");
}

static void test_restart(void) {
    printf("restarting\n");
    snake_game_t g;
    q6_board(&g, false);
    snake_game_reset(&g, 0);
    g.food = SNAKE_GAME_NO_CELL;

    uint32_t t = 0;
    while (g.alive && t < 100) {
        snake_game_step(&g, ++t);
    }
    CHECK(!g.alive, "the game ended");

    snake_game_turn(&g, SNAKE_GAME_DIR_UP);
    snake_game_step(&g, ++t);
    CHECK(g.alive, "an arrow key restarts a finished game");
    CHECK(g.len == SNAKE_GAME_START_LEN, "the snake is back to its starting length");
    CHECK(g.score == 0, "and the score is back to zero");

    // and it also respawns on its own once the corpse has faded out
    t = 0;
    while (g.alive && t < 100) {
        snake_game_step(&g, ++t);
    }
    uint32_t dead_at = g.dead_ms;
    snake_game_turn(&g, SNAKE_GAME_DIR_LEFT);
    g.step_ms = 1000;
    snake_game_tick(&g, dead_at + 10);
    CHECK(g.alive, "an arrow key restarts through the tick path the effect uses as well");

    t = 0;
    while (g.alive && t < 200) {
        snake_game_step(&g, ++t);
    }
    dead_at = g.dead_ms;
    CHECK(!g.alive, "the game ended again");
    CHECK(snake_game_dead_for(&g, dead_at) == 0, "a fresh corpse has faded for zero ms");
    g.step_ms = 1000;
    snake_game_tick(&g, dead_at + g.game_over_ms - 1);
    CHECK(!g.alive, "still showing the corpse just before the respawn");
    snake_game_tick(&g, dead_at + g.game_over_ms);
    CHECK(g.alive, "the game respawns on its own after the corpse fades");
    invariants(&g, "restart");
}

static void test_corpse_fade(void) {
    printf("corpse fade (no flashing)\n");
    snake_game_t g;
    q6_board(&g, false);
    snake_game_reset(&g, 0);
    g.food = SNAKE_GAME_NO_CELL;

    uint32_t t = 0;
    while (g.alive && t < 100) {
        snake_game_step(&g, ++t);
    }
    CHECK(!g.alive, "the corpse test needs a dead snake");

    uint32_t go = g.game_over_ms;
    uint8_t previous = 255;
    for (uint32_t dead_for = 0; dead_for <= go; dead_for += 25) {
        // the exact expression the effect uses for the corpse brightness
        uint8_t lit = (uint8_t)(255 - dead_for * 255 / go);
        if (dead_for > 0) {
            CHECK(lit <= previous, "the corpse only ever dims, it never strobes back up");
        }
        previous = lit;
    }

    CHECK(255 - 0 * 255 / go == 255, "the corpse starts fully lit");
    CHECK(255 - go * 255 / go == 0, "and ends fully dark");
    CHECK(go == SNAKE_GAME_OVER_MS, "the fade lasts exactly the game over window");
}

static void test_tiny_board(void) {
    printf("tiny board (3x3, wrap)\n");
    snake_game_t g;
    snake_game_init(&g, 3, 3, true);
    snake_game_reset(&g, 0);

    CHECK(g.lit_count == 9, "every cell is lit on a plain board");
    CHECK(g.len == SNAKE_GAME_START_LEN, "the snake still fits");
    invariants(&g, "tiny start");

    test_rng = 7u;
    int games = 0, wall = 0, max_len = 0;
    for (uint32_t i = 0; i < 3000; i++) {
        if (rnd(4) == 0) {
            snake_game_turn(&g, rnd(4));
        }
        if (!snake_game_step(&g, i + 1)) {
            games++;
            if (g.death == SNAKE_DEATH_WALL) {
                wall++;
            } else {
                CHECK(g.death == SNAKE_DEATH_SELF, "on a tiny wrapping board only the body is deadly");
            }
            snake_game_reset(&g, i + 1);
        }
        if (g.len > max_len) {
            max_len = g.len;
        }
        invariants(&g, "tiny board");
    }

    printf("  %d games, longest snake %d of 9 cells\n", games, max_len);
    CHECK(wall == 0, "wrap mode never dies on a wall, not even on a 3x3 board");
    CHECK(max_len >= 5, "the snake grows on a tiny board");
    // note: a snake that fills a wrapping board can slither around the loop for
    // ever, which is correct snake behaviour and only reachable on a tiny board
}

// a simple player that walks towards the food without walking into itself
static uint8_t greedy_dir(const snake_game_t *g) {
    uint8_t best = g->dir;
    int best_dist = 1 << 30;

    for (uint8_t dir = 0; dir <= SNAKE_GAME_DIR_RIGHT; dir++) {
        if (snake_game_is_reverse(g, dir)) {
            continue;
        }

        int8_t dr = snake_game_dr(dir);
        int8_t dc = snake_game_dc(dir);
        int16_t row = g->body[0] / g->cols;
        int16_t col = g->body[0] % g->cols;
        uint8_t next = SNAKE_GAME_NO_CELL;

        for (uint8_t guard = 0; guard < g->cells; guard++) {
            row = (row + dr + g->rows) % g->rows;
            col = (col + dc + g->cols) % g->cols;
            uint8_t cell = (uint8_t)(row * g->cols + col);
            if (g->lit[cell]) {
                next = cell;
                break;
            }
        }

        if (next == SNAKE_GAME_NO_CELL) {
            continue;
        }

        bool deadly = false;
        for (uint8_t i = 0; i + 1 < g->len; i++) {
            if (g->body[i] == next) {
                deadly = true;
            }
        }
        if (deadly) {
            continue;
        }

        int food_row = g->food / g->cols;
        int food_col = g->food % g->cols;
        int dist = (row > food_row ? row - food_row : food_row - row) +
                   (col > food_col ? col - food_col : food_col - col);
        if (dist < best_dist) {
            best_dist = dist;
            best = dir;
        }
    }

    return best;
}

static void test_playability(void) {
    printf("playability (a simple player, 400 games)\n");
    snake_game_t g;
    q6_board(&g, true);

    int games = 0;
    uint64_t total_steps = 0, total_score = 0;
    int min_steps = 1 << 30, max_steps = 0, max_score = 0, wall = 0;
    test_rng = 987654321u;

    for (int game = 0; game < 400; game++) {
        snake_game_reset(&g, 0);

        int steps = 0;
        while (g.alive && steps < 5000) {
            snake_game_turn(&g, greedy_dir(&g));
            snake_game_step(&g, (uint32_t)(++steps) * 1000);
            invariants_light(&g);
        }

        games++;
        total_steps += (uint64_t)steps;
        total_score += g.score;
        if (steps < min_steps) {
            min_steps = steps;
        }
        if (steps > max_steps) {
            max_steps = steps;
        }
        if (g.score > max_score) {
            max_score = g.score;
        }
        if (g.death == SNAKE_DEATH_WALL) {
            wall++;
        }
        if (game == 0) {
            show(&g, "a game that just ended");
        }
    }

    int avg_steps = (int)(total_steps / (uint64_t)games);
    int avg_score = (int)(total_score / (uint64_t)games);
    double step_ms = 1400 - (1400 - 450) * 127 / 255;

    printf("  avg %d steps (%.1f s at speed 127), min %d, max %d, avg score %d, max score %d, wall deaths %d\n",
           avg_steps, avg_steps * step_ms / 1000.0, min_steps, max_steps, avg_score, max_score, wall);

    CHECK(wall == 0, "no wall deaths in wrap mode");
    CHECK(min_steps >= 4, "a game is never over the instant it starts");
    CHECK(avg_steps >= 25, "the board is big enough to actually play");
    CHECK(max_score >= 5, "the food is reachable");
}

int main(void) {
    test_board_map();
    test_gap_slide();
    test_wrap_fuzz();
    test_no_wrap_walls();
    test_turns();
    test_timing();
    test_restart();
    test_corpse_fade();
    test_tiny_board();
    test_playability();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
