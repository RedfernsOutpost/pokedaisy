/* The Mac's menu bar: File (Add Games..., Show Games Folder), View (Search, cover size, Full Screen), Game (Game Menu,
 * Save / Load State, Screenshot), Help (releases), and Settings... in the app menu. Each item posts an SDL_USEREVENT
 * whose code is one of the host's CMD_ values; Add Games... hands each file picked to add_game. */
#ifndef PD_MAC_MENU_H
#define PD_MAC_MENU_H

void mac_menu_install(void (*add_game)(const char *path), const int *codes);

#endif
