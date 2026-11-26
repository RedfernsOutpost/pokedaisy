/* The Mac's menu bar: File (Add Games..., Show Games Folder), View (Search, cover size, Full Screen), Game (Game Menu,
 * Save / Load State, Screenshot), Help (releases), and Settings... in the app menu. Each item posts an SDL_USEREVENT
 * whose code is one of the host's CMD_ values; Add Games... hands each file picked to add_game.
 * Also the app updater's Mac parts: where the running app is, whose signature it carries, and the swap. */
#ifndef PD_MAC_MENU_H
#define PD_MAC_MENU_H

#include <stdbool.h>
#include <stddef.h>

void mac_menu_install(void (*add_game)(const char *path), const int *codes);

/* The running app's bundle (".../Daisy Launcher.app"); false when it isn't run from a bundle. */
bool mac_bundle_path(char *out, size_t len);
/* True when the bundle at [app] is validly signed (strict, nested code included) by the same Developer ID team as the
 * running app. False when the running app has no team (an ad hoc build): such a build can't update itself. */
bool mac_same_signer(const char *app);
/* Puts [current] (the running app) in the Trash and [fresh] in its place; on failure [current] is put back. */
bool mac_swap_app(const char *current, const char *fresh);

#endif
