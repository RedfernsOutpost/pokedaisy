/* The game list as the app's library (LibraryActivity): GAMES / TOOLS / SETTINGS down a rail on the left, the POKéDAISY
 * header, the quick views as coloured pills with their counts (ALL, FAVORITES, RECENT, the consoles, HACKS), the games
 * as a grid of covers or a list, a game's menu on B and the button legend in the corner. Immediate mode like the
 * companion: the host feeds the keys pressed each frame and draws it into its canvases.
 *
 * Covers are the firmware's scraped box art (EmulationStation's gamelist.xml / images/), decoded off the UI thread; the
 * host draws them at its own resolution into the wells library_covers() reports, between library_draw (the page) and
 * library_draw_top (the legend, the menus). Names come from the app's game table by the ROM's SHA-1 (hashed in the
 * background, kept in DATA/library-titles.txt), else from the file name. Favorites, last played, the view, the order and
 * grid / list are DATA/library.txt; DARK MODE, THEME and ASPECT are the cores' own settings file (one look everywhere). */
#ifndef POCKET_LIBRARY_H
#define POCKET_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#include "pd_canvas.h"

enum {
  LIB_A = 1 << 0, LIB_B = 1 << 1, LIB_X = 1 << 2, LIB_Y = 1 << 3, LIB_L = 1 << 4, LIB_R = 1 << 5, LIB_UP = 1 << 6,
  LIB_DOWN = 1 << 7, LIB_LEFT = 1 << 8, LIB_RIGHT = 1 << 9, LIB_START = 1 << 10, LIB_SELECT = 1 << 11, LIB_MENU = 1 << 12,
};

/* What the host adds to SETTINGS: its own rows (FF SPEED, REWIND, ...), each a label, its value now and a step. */
struct lib_host {
  int settings; // how many rows
  const char *(*label)(int row);
  const char *(*group)(int row); // the group it sits in (SCREEN, FF / SPEED, CONTROLS)
  void (*value)(int row, char *out, int len);
  void (*step)(int row, int dir); // dir: +1 / -1
  const char *version;
};

/* A cover's place this frame: canvas pixels, the clip it's drawn within, its square of pixels (size x size). */
struct lib_cover {
  int x, y, w, h;
  int clipX, clipY, clipW, clipH;
  int game;        // which game's (the host keys its texture on it)
  unsigned serial; // changes when that game's pixels do
  const uint32_t *px;
  int size;
};

/* The games, DATA/roms' first (a game in two folders is listed once, from the first); each folder's gamelist.xml is read
 * for its covers, favorites and last played. */
void library_open(char *const *paths, int count, const char *dataDir, const char *savesDir, const struct lib_host *host);
void library_keys(unsigned down);
/* A pointer press or wheel notch on the canvas library_draw gets: (x, y) canvas pixels, click = a left press, wheel > 0 = up. */
void library_pointer(int x, int y, bool click, int wheel);
/* A right click (or control-click) at (x, y): the game under it is picked and its menu opens; elsewhere a menu closes. */
void library_context(int x, int y);
/* Something dropped on the window (a file path or URL, an image's web address): the picked game's cover. */
void library_drop(const char *what);
/* Every frame: where the pointer is and whether the left button is down, for a scroll bar being dragged. */
void library_hold(int x, int y, bool held);
/* Cmd+F: the search box, and the keys it takes (a character, '\b', '\r' done, 27 clear) while library_searching(). */
bool library_searching(void);
void library_search_open(void);
void library_search_key(int ch);
/* Cmd+, : SETTINGS. Cmd+ / Cmd- / Cmd+0: bigger, smaller, usual covers. */
void library_settings_open(void);
void library_zoom(int dir);
/* Games were added (dropped on the window): the host lists its folders again (library_close, then library_open). */
bool library_reload_wanted(void);
/* A note from the host along the bottom (a controller it can't use), for a few seconds. */
void library_say(const char *text);
void library_tick(void);
void library_draw(struct pd_canvas *c);
int library_covers(struct lib_cover *out, int max);
void library_draw_top(struct pd_canvas *c, uint32_t clear);
/* A menu or a question is up: the host darkens the covers too before library_draw_top. */
bool library_dimmed(void);
/* GAME SETTINGS was picked in a game's menu: that game's path until library_game_settings_done(), else NULL. */
const char *library_game_settings(void);
void library_game_settings_done(void);
/* A game to play (then cleared), NULL = none. */
const char *library_picked(void);
/* The player closed the app. */
bool library_quit(void);
/* A game is playing: the background work (hashing, cover downloads) waits until it's false again. */
void library_pause(bool paused);
/* Back from a game: it's the last played, the cursor on it, its save looked for again. */
void library_played(const char *path);
void library_close(void);

#endif
