/* PokeDaisy Linux: an SDL2 frontend for the PokeDaisy libretro cores. The companion, HUD, quick menu, FLY / PC /
 * REPEL / BIKE and EXPAND are the cores' own (libretro/, libretro/armgba/), so this host shows exactly what the
 * cores do on every other frontend. It loads the core for the ROM from beside the binary
 * (armGBA for .gba, mGBA for .gb / .gbc, DSperate for .nds), tells it the screen (the pokedaisy_panel option: the core then picks its canvas), shows its frame at a whole scale, and owns
 * what a frontend owns: input, sound, pacing, the battery save and its backups, save states, fast-forward,
 * rewind, the LCD mask and the game list. */
#include "libretro.h"
#include "../3ds/desktop/pd_png.h"
#include "display_filter.h"
#include "pd_canvas.h"
#include "pd_menu.h"
#include <SDL.h>
#include <dirent.h>
#ifdef _WIN32
#include "win32_dl.h"
#else
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "pd_fs.h"
#include "pd_env.h"
#include "pd_zip.h"

#include "library.h"
#ifdef __APPLE__
#include "mac_menu.h"
#endif
#ifndef _WIN32
#include <glob.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

/* The cores' file ending: dlopen'd .so objects on Linux, .dll on Windows, .dylib on macOS. */
#ifdef _WIN32
#include "SDL_syswm.h"
#include <windows.h>
#define PD_CORE_EXT ".dll"
#define PD_HOST_NAME "PokeDaisy"
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#define PD_CORE_EXT ".dylib"
#define PD_HOST_NAME "PokeDaisy"
#else
#define PD_CORE_EXT ".so"
#define PD_HOST_NAME "PokeDaisy Linux"
#endif

// The window's size without POCKET_FULLSCREEN / POCKET_WINDOW: the app's handheld panel, 4:3.
#define W 512
#define H 384
#define OVERLAY_MAX_W 1024
#define OVERLAY_MAX_H 768
#define KEY_COLOR 0xFF00FFu // the overlay's see-through colour
#define OUT_RATE 48000
#define MODIFIER_TAP_FRAMES 6 // a tap of the MODIFIER alone reaches the game this long (Game Boy menus miss 1-frame presses)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_AudioDeviceID audio;
static volatile sig_atomic_t running = 1;
static int appError;
static const char *root = ".";
/* plorpOS: --save FILE, the battery save another emulator keeps (its backups then go to DATA/backups);
 * NULL = DATA/saves/ROM.sav, or the save named after the ROM without its extension. */
static const char *saveFile;
static char savesDir[PATH_MAX], exeDir[PATH_MAX];
static char message[128];
static unsigned messageUntil;

#ifdef _WIN32
/* The icon the executable carries (windows/pokedaisy.rc): the title bar, the taskbar and Alt-Tab take it here, so the
 * window does not depend on SDL picking the exe's own. */
static void set_window_icon(SDL_Window *win) {
  if (!win) return;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS || !info.info.win.window) return;
  HICON big = (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), LR_SHARED);
  HICON small = (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  if (big) SendMessage(info.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)big);
  if (small) SendMessage(info.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)small);
}
#endif

static void stop(int sig) {
  (void)sig;
  running = 0;
}
static void notice(const char *s) {
  snprintf(message, sizeof message, "%s", s);
  messageUntil = SDL_GetTicks() + 2500;
  fprintf(stderr, "%s\n", s);
}

/* ---- input ---- */
enum {
  A,
  B,
  SELECT,
  START,
  RIGHT,
  LEFT,
  UP,
  DOWN,
  R,
  L,
  X,
  Y,
  L2,
  R2,
  MENU,
  L3,
  SUP, // the left stick's directions apart (they're also the D-pad's)
  SDOWN,
  SLEFT,
  SRIGHT,
  R3,
  P1, // the back paddles (the Steam Deck's L4 / R4 / L5 / R5, an Elite pad's), when SDL is given them
  P2,
  P3,
  P4,
  FFKEY, // the keyboard's own: fast-forward held, rewind held, save / load state
  RWKEY,
  SAVEKEY,
  LOADKEY,
  NKEYS
};
static SDL_GameController *controller; // the pad last pressed (its name in SETTINGS); every pad in pads[] reaches the game
static SDL_GameController *pads[8];
static int padCount;
static bool rawTrimui;
static bool swapFace; // the host menu's A/B BUTTONS: SWAPPED (host.ini swap_ab)
/* The PANEL presets: the screen the core is told to lay its canvas out for. 0 is the window's own size. */
#define PD_PANEL_PRESETS 5
static const char *panel_preset(int panel) {
  static const char *const panels[PD_PANEL_PRESETS] = {NULL, "1280x720", "1280x800", "1920x1080", "2560x1440"};
  return panel > 0 && panel < PD_PANEL_PRESETS ? panels[panel] : NULL;
}
// A controller's face button, after the host menu's A/B BUTTONS (the keyboard's keys stay where they are).
static int face(int k) {
  if (!swapFace)
    return k;
  return k == A ? B : k == B ? A : k == X ? Y : k == Y ? X : k;
}
static unsigned held, pressed;
static bool inputReset;
static unsigned repeatAt[NKEYS];
/* The keyboard's keys, one per action: the host menu's KEYBOARD page sets them, kept in host.ini (kb_*). A floor of a
 * keyboard can't follow a game pad's layout, so each action has its own key. SDL keycodes; SDLK_UNKNOWN = the row is off. */
enum {
  KB_A, KB_B, KB_X, KB_Y, KB_L, KB_R, KB_L2, KB_R2, KB_START, KB_SELECT,
  KB_UP, KB_DOWN, KB_LEFT, KB_RIGHT, KB_MENU, KB_FF, KB_REWIND, KB_SAVE, KB_LOAD, KB_COUNT
};
static const char *const kbIds[KB_COUNT] = {"a",      "b",     "x",      "y",    "l1", "r1",     "l2",     "r2",
                                            "start",  "select", "up",    "down", "left", "right", "menu", "ff",
                                            "rewind", "savestate", "loadstate"};
static const char *const kbLabels[KB_COUNT] = {"A",   "B",   "X",     "Y",    "L1",   "R1",     "L2",    "R2",
                                               "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT", "MENU",
                                               "FAST FORWARD", "REWIND", "SAVE STATE", "LOAD STATE"};
static const int kbTargets[KB_COUNT] = {A,    B,    X,   Y,    L,      R,      L2,     R2,      START,   SELECT,
                                        UP,   DOWN, LEFT, RIGHT, MENU,  FFKEY,  RWKEY,  SAVEKEY, LOADKEY};
static int kbKeys[KB_COUNT] = {SDLK_z,   SDLK_x,       SDLK_TAB,  SDLK_f,     SDLK_q,     SDLK_e,   SDLK_1,
                               SDLK_2,   SDLK_RETURN,  SDLK_BACKSPACE, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
                               SDLK_ESCAPE, SDLK_SPACE, SDLK_r,   SDLK_F2,    SDLK_F4};
static int kbCapture = -1; // the KEYBOARD page's row waiting for a key, -1 none
static int lastKey;        // the key that went down this frame (that page reads it)
static int keyboard(SDL_Keycode k) {
  for (int i = 0; i < KB_COUNT; i++)
    if (kbKeys[i] && kbKeys[i] == (int)k)
      return kbTargets[i];
  return -1;
}
/* The KEYBOARD page: the in-game host menu's and the game list's SETTINGS page, one list of the same keys. */
static char kbRows[KB_COUNT][48];
static const char *kbPtrs[KB_COUNT];
static struct pd_menu kbMenu;
static void kb_menu_build(void) {
  for (int i = 0; i < KB_COUNT; i++) {
    const char *name = kbKeys[i] ? SDL_GetKeyName((SDL_Keycode)kbKeys[i]) : "OFF";
    snprintf(kbRows[i], sizeof kbRows[i], "%s  %s", kbLabels[i], kbCapture == i ? "..." : name);
    kbPtrs[i] = kbRows[i];
  }
}
static void kb_menu_open(void) {
  kb_menu_build();
  pd_menu_init(&kbMenu, "KEYBOARD", kbPtrs, KB_COUNT);
  kbMenu.overlay = true;
}
/* POCKET_FACE_LABELS=1: the controller mapping already names the face buttons by their labels (ROCKNIX's PortMaster mapping in
 * its Nintendo layout: SDL's "a" is the button marked A, wherever it sits), so A is A. Else SDL's are an Xbox pad's places
 * (A at the bottom) and the game's follow the GBA's: the bottom one is B, the right one A. */
static bool faceLabels;
static int controller_button(int button) {
  if (faceLabels)
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
      return A;
    case SDL_CONTROLLER_BUTTON_B:
      return B;
    case SDL_CONTROLLER_BUTTON_X:
      return X;
    case SDL_CONTROLLER_BUTTON_Y:
      return Y;
    default:
      break;
    }
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A:
    return B;
  case SDL_CONTROLLER_BUTTON_B:
    return A;
  case SDL_CONTROLLER_BUTTON_X:
    return Y;
  case SDL_CONTROLLER_BUTTON_Y:
    return X;
  case SDL_CONTROLLER_BUTTON_BACK:
    return SELECT;
  case SDL_CONTROLLER_BUTTON_START:
    return START;
  case SDL_CONTROLLER_BUTTON_GUIDE:
    return MENU;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return R3;
  case SDL_CONTROLLER_BUTTON_PADDLE1:
    return P1;
  case SDL_CONTROLLER_BUTTON_PADDLE2:
    return P2;
  case SDL_CONTROLLER_BUTTON_PADDLE3:
    return P3;
  case SDL_CONTROLLER_BUTTON_PADDLE4:
    return P4;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return L;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return R;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return RIGHT;
  default:
    return -1;
  }
}
static int joy(int n) {
  static const int map[] = {B, A, Y, X, L, R, SELECT, START, MENU, -1, L2, R2};
  return n >= 0 && n < 12 ? map[n] : -1;
}
/* A direction is held by the D-pad (or the keyboard, the hat) and by the left stick apart: letting go of one keeps it
 * held while the other still has it. */
static unsigned padHeld;  // directions the D-pad, keyboard or hat hold
static bool fromStick;    // key() called by stick_dir
static int stick_of(int dir) { return dir == UP ? SUP : dir == DOWN ? SDOWN : dir == LEFT ? SLEFT : dir == RIGHT ? SRIGHT : -1; }
static void key(int k, bool isDown) {
  if (k < 0)
    return;
  unsigned bit = 1u << k;
  int stick = stick_of(k);
  if (stick >= 0) {
    if (fromStick) {
      if (!isDown && (padHeld & bit))
        return; // the D-pad still holds it
    } else {
      padHeld = isDown ? padHeld | bit : padHeld & ~bit;
      if (!isDown && (held & (1u << stick)))
        return; // the stick still holds it
    }
  }
  if (isDown) {
    if (!(held & bit)) {
      pressed |= bit;
      repeatAt[k] = SDL_GetTicks() + 400;
    }
    held |= bit;
  } else
    held &= ~bit;
}
/* The left stick is the D-pad too, but only when it crosses the threshold: a stick at rest still sends small axis
 * events (the Steam Deck's, all the time), and releasing the direction on each of them let go of a held D-pad. */
static void stick_dir(int stickKey, int dir, bool on) {
  if (on == ((held & (1u << stickKey)) != 0))
    return;
  key(stickKey, on);
  fromStick = true;
  key(dir, on);
  fromStick = false;
}
/* Controllers come and go (a USB or Bluetooth pad, a handheld whose built-in one shows up late): SDL reports the
 * ones it has a mapping for as game controllers (its own database, DATA/gamecontrollerdb.txt, or
 * SDL_GAMECONTROLLERCONFIG, which PortMaster sets). The first one is used; an unmapped device is named in the log
 * so a mapping can be added. */
static char padWarning[160]; // said on screen at the next frame (the list or the game)
static char unmappedPad[64]; // the last pad without a mapping, for SETTINGS > CONTROLLER
static void controller_added(int index) {
  if (!SDL_IsGameController(index)) {
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
    fprintf(stderr,
            "Controller without a mapping: %s (%s); add one to "
            "DATA/gamecontrollerdb.txt or SDL_GAMECONTROLLERCONFIG\n",
            SDL_JoystickNameForIndex(index) ? SDL_JoystickNameForIndex(index) : "?", guid);
    // On screen too: a pad with no mapping otherwise just looks broken.
    snprintf(unmappedPad, sizeof unmappedPad, "%s", SDL_JoystickNameForIndex(index) ? SDL_JoystickNameForIndex(index) : "A controller");
    snprintf(padWarning, sizeof padWarning, "%s isn't supported yet (no button map for it)", unmappedPad);
    return;
  }
  // Every pad is opened, not only the first: a stale virtual pad (one an iPhone controller app left behind) listed
  // first would otherwise take the place of the one being pressed.
  SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
  for (int i = 0; i < padCount; i++)
    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id)
      return;
  if (padCount == (int)(sizeof pads / sizeof *pads))
    return;
  SDL_GameController *pad = SDL_GameControllerOpen(index);
  if (!pad)
    return;
  pads[padCount++] = pad;
  if (!controller)
    controller = pad;
  unmappedPad[0] = 0;
  fprintf(stderr, "Controller: %s\n", SDL_GameControllerName(pad));
}
static void controller_removed(SDL_JoystickID id) {
  int at = -1;
  for (int i = 0; i < padCount; i++)
    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id)
      at = i;
  if (at < 0)
    return;
  SDL_GameController *gone = pads[at];
  pads[at] = pads[--padCount];
  SDL_GameControllerClose(gone);
  if (controller == gone)
    controller = padCount ? pads[0] : NULL;
  inputReset = true;
  held = 0; // nothing stays pressed on a pad that's gone
}
/* The pointer (a touch screen, or the mouse): where on the core's frame, for RETRO_DEVICE_POINTER. */
static SDL_Rect frameRect;
/* A core's native touch panel: the bottom screen at 256 x 192, scaled straight to its output rectangle. */
static struct pd_touch_panel panelMsg;
static bool panelPending, panelActive;
static SDL_Texture *panelTexture;
static int panelTexW, panelTexH;
/* A core's field HUD at its own resolution, drawn over its box at the window's pixels (a ZOOM step scales the canvas). */
static struct pd_hud hudMsg;
static bool hudPending, hudActive, hudHave; // hudHave: this frame sent one (a frame shown again keeps the last)
static SDL_Texture *hudTexture;
static int hudTexW, hudTexH;
static int pointerX, pointerY;
static bool pointerDown;
static void script_mouse(void);
static bool pointerHeld; // the left button is down (pointerDown is that, held for at least a frame)
static int pointerPulse;
static int wheelSteps;     // the wheel's notches this frame (+ up), for RETRO_DEVICE_MOUSE
static int mouseWindowX, mouseWindowY; // the pointer over the window, for the library (the cores get it normalised)
static bool libraryClick;              // a left press this frame
static bool libraryRightClick;         // a right press (or control-click) this frame: a game's menu
static char libraryDropped[PATH_MAX * 2]; // a file or an image's address dropped on the window: the picked game's cover
/* Files picked in the menu bar's File > Add Games... (added to DATA/roms when the game list is up). */
static char addQueue[16][PATH_MAX];
static int addQueued;
static void menu_add_game(const char *path) {
  const char *ext = strrchr(path, '.');
  if (!ext || (strcasecmp(ext, ".gb") && strcasecmp(ext, ".gbc") && strcasecmp(ext, ".gba") && strcasecmp(ext, ".nds") &&
               strcasecmp(ext, ".zip")) || addQueued >= 16)
    return;
  snprintf(addQueue[addQueued++], PATH_MAX, "%s", path);
}
static bool windowAway;                // another window has the focus (PAUSE IN BACKGROUND)
static bool listShown;                 // the game list is up (not a game): its shortcuts and search take keys
static int libraryWheel;               // wheel notches this frame (+ up)

static void pointer_at(int x, int y) {
  if (frameRect.w <= 0 || frameRect.h <= 0)
    return;
  int px = (int)((long)(x - frameRect.x) * 0xFFFE / frameRect.w) - 0x7FFF;
  int py = (int)((long)(y - frameRect.y) * 0xFFFE / frameRect.h) - 0x7FFF;
  pointerX = px < -0x7FFF ? -0x7FFF : px > 0x7FFF ? 0x7FFF : px;
  pointerY = py < -0x7FFF ? -0x7FFF : py > 0x7FFF ? 0x7FFF : py;
}
static bool audioLost, outputChanged;
static int key_named(const char *s);
/* POCKET_KEYS="TICK:KEY[:HOLD],...": presses scripted by loop tick (each events() call), held HOLD ticks (2), for
 * tests: KEY is a key name (a, b, up, menu, start, l3, r2 ...: keyNames). */
static struct {
  unsigned at, hold;
  int key;
} script[256];
static int scriptN = -1;
static unsigned tick;
static void key(int k, bool isDown);
static void script_resize(void);
static void script_keys(void) {
  if (scriptN < 0) {
    scriptN = 0;
    char buf[1024], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_KEYS") ? getenv("POCKET_KEYS") : "");
    for (char *t = strtok_r(buf, ",", &save); t && scriptN < 256; t = strtok_r(NULL, ",", &save)) {
      char name[16];
      unsigned at, hold = 2;
      if (sscanf(t, "%u:%15[^:]:%u", &at, name, &hold) >= 2 && key_named(name) >= 0)
        script[scriptN++] = (typeof(script[0])){at, hold, key_named(name)};
    }
  }
  for (int i = 0; i < scriptN; i++) {
    if (tick == script[i].at)
      key(script[i].key, true);
    if (tick == script[i].at + script[i].hold)
      key(script[i].key, false);
  }
  tick++;
}
/* POCKET_RAWKEY="TICK:KEYCODE[:HOLD]", a real SDL key event at that tick: the KEYBOARD page's capture, tested. */
static struct {
  unsigned at, hold;
  int key;
} rawscript[16];
static int rawscriptN = -1;
static void script_rawkeys(void) {
  if (rawscriptN < 0) {
    rawscriptN = 0;
    char buf[256], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_RAWKEY") ? getenv("POCKET_RAWKEY") : "");
    for (char *t = strtok_r(buf, ",", &save); t && rawscriptN < 16; t = strtok_r(NULL, ",", &save)) {
      unsigned at, hold = 2;
      int code;
      if (sscanf(t, "%u:%d:%u", &at, &code, &hold) >= 2)
        rawscript[rawscriptN++] = (typeof(rawscript[0])){at, hold, code};
    }
  }
  for (int i = 0; i < rawscriptN; i++) {
    if (tick == rawscript[i].at || tick == rawscript[i].at + rawscript[i].hold) {
      SDL_Event e = {0};
      e.type = tick == rawscript[i].at ? SDL_KEYDOWN : SDL_KEYUP;
      e.key.type = e.type;
      e.key.state = tick == rawscript[i].at ? SDL_PRESSED : SDL_RELEASED;
      e.key.keysym.sym = (SDL_Keycode)rawscript[i].key;
      SDL_PushEvent(&e);
    }
  }
}
/* Keys that aren't buttons: Cmd+Ctrl+F or F11 (fullscreen) anywhere; in the game list Cmd+F (search), Cmd+, (SETTINGS),
 * Cmd + / Cmd - / Cmd+0 (cover size), and while the search box is open, the keys it types. True when taken. */
/* What the keys below and the Mac's menu bar (mac_menu.m, as SDL_USEREVENTs) ask for. */
enum { CMD_FULLSCREEN = 1, CMD_SEARCH, CMD_SETTINGS, CMD_BIGGER, CMD_SMALLER, CMD_ACTUAL, CMD_SCREENSHOT,
       CMD_GAMES_FOLDER, CMD_GAME_MENU, CMD_SAVE_STATE, CMD_LOAD_STATE, CMD_RELEASES };
static bool screenshotWanted; // the game loop saves the next frame to Pictures > PokeDaisy
static bool command(int c) {
  if (c == CMD_FULLSCREEN) {
    bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    return true;
  }
  if (c == CMD_GAMES_FOLDER || c == CMD_RELEASES) {
#ifndef _WIN32
    char dir[PATH_MAX + 8];
    snprintf(dir, sizeof dir, "%s/roms", root);
#ifdef __APPLE__
    const char *argv[] = {"open", c == CMD_RELEASES ? "https://github.com/RedfernsOutpost/pokedaisy/releases/latest" : dir, NULL};
#else
    const char *argv[] = {"xdg-open", c == CMD_RELEASES ? "https://github.com/RedfernsOutpost/pokedaisy/releases/latest" : dir, NULL};
#endif
    pid_t pid;
    if (!posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ))
      waitpid(pid, NULL, 0);
#endif
    return true;
  }
  if (!listShown) { // in a game
    if (c == CMD_SCREENSHOT)
      screenshotWanted = true;
    else if (c == CMD_GAME_MENU)
      pressed |= 1u << MENU;
    else if (c == CMD_SAVE_STATE)
      pressed |= 1u << SAVEKEY;
    else if (c == CMD_LOAD_STATE)
      pressed |= 1u << LOADKEY;
    else
      return false;
    return true;
  }
  if (c == CMD_SEARCH)
    library_search_open();
  else if (c == CMD_SETTINGS)
    library_settings_open();
  else if (c == CMD_BIGGER || c == CMD_SMALLER || c == CMD_ACTUAL)
    library_zoom(c == CMD_BIGGER ? 1 : c == CMD_SMALLER ? -1 : 0);
  else
    return false;
  return true;
}
static bool shortcut(const SDL_KeyboardEvent *k) {
  SDL_Keycode sym = k->keysym.sym;
  Uint16 mod = k->keysym.mod;
  bool cmd = (mod & KMOD_GUI) != 0;
  if (sym == SDLK_F11 || (cmd && (mod & KMOD_CTRL) && sym == SDLK_f))
    return k->repeat || command(CMD_FULLSCREEN);
  if (!listShown) // in a game: F12 or Cmd+S, a screenshot
    return (sym == SDLK_F12 || (cmd && sym == SDLK_s)) && (k->repeat || command(CMD_SCREENSHOT));
  if (cmd) {
    int c = sym == SDLK_f ? CMD_SEARCH : sym == SDLK_COMMA ? CMD_SETTINGS
          : sym == SDLK_EQUALS || sym == SDLK_PLUS || sym == SDLK_KP_PLUS ? CMD_BIGGER
          : sym == SDLK_MINUS || sym == SDLK_KP_MINUS ? CMD_SMALLER : sym == SDLK_0 ? CMD_ACTUAL : 0;
    return c && command(c); // Cmd+Q and the rest: the system's
  }
  if (!library_searching())
    return false;
  if (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT)
    return false; // still move through what's found
  if (sym == SDLK_ESCAPE)
    library_search_key(27);
  else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
    library_search_key('\r');
  else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE)
    library_search_key('\b');
  else if (sym >= 32 && sym < 127)
    library_search_key((int)sym);
  return true; // the box has the keyboard
}

static void events(void) {
  pressed = 0;
  lastKey = 0;
  if (padWarning[0]) { // a controller with no mapping came in (last frame): the list's notice or the game's
    if (listShown)
      library_say(padWarning);
    else
      notice(padWarning);
    padWarning[0] = 0;
  }
  script_rawkeys();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT)
      running = 0;
    if (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))
      controller_added(e.jdevice.which);
    if (e.type == SDL_AUDIODEVICEREMOVED && !e.adevice.iscapture && audio && e.adevice.which == audio)
      audioLost = true;
    if (e.type == SDL_CONTROLLERDEVICEADDED)
      controller_added(e.cdevice.which);
    if (e.type == SDL_CONTROLLERDEVICEREMOVED)
      controller_removed(e.cdevice.which);
    if (e.type == SDL_KEYDOWN && shortcut(&e.key))
      continue; // Cmd shortcuts, fullscreen, the game list's search: not a button
    if (e.type == SDL_KEYDOWN && kbCapture >= 0)
      lastKey = (int)e.key.keysym.sym; // the KEYBOARD page's next key: the game sees nothing of it
    else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat)
      key(keyboard(e.key.keysym.sym), e.type == SDL_KEYDOWN);
    if (e.type == SDL_CONTROLLERBUTTONDOWN) { // the pad pressed is the one in use
      SDL_GameController *pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
      if (pad)
        controller = pad;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP)
      key(face(controller_button(e.cbutton.button)), e.type == SDL_CONTROLLERBUTTONDOWN);
    if (e.type == SDL_CONTROLLERAXISMOTION) {
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
        key(L2, e.caxis.value > 16000);
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        key(R2, e.caxis.value > 16000);
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        stick_dir(SLEFT, LEFT, e.caxis.value < -16000);
        stick_dir(SRIGHT, RIGHT, e.caxis.value > 16000);
      }
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        stick_dir(SUP, UP, e.caxis.value < -16000);
        stick_dir(SDOWN, DOWN, e.caxis.value > 16000);
      }
    }
    if (!controller && rawTrimui && (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP))
      key(face(joy(e.jbutton.button)), e.type == SDL_JOYBUTTONDOWN);
    if (!controller && rawTrimui && e.type == SDL_JOYHATMOTION) {
      key(UP, e.jhat.value & SDL_HAT_UP);
      key(DOWN, e.jhat.value & SDL_HAT_DOWN);
      key(LEFT, e.jhat.value & SDL_HAT_LEFT);
      key(RIGHT, e.jhat.value & SDL_HAT_RIGHT);
    }
    if (!controller && rawTrimui && e.type == SDL_JOYAXISMOTION) {
      if (e.jaxis.axis == 2)
        key(L2, e.jaxis.value > 16000);
      if (e.jaxis.axis == 5)
        key(R2, e.jaxis.value > 16000);
    }
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
      mouseWindowX = e.button.x, mouseWindowY = e.button.y;
      pointer_at(e.button.x, e.button.y);
      pointerHeld = e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
      if (e.type == SDL_MOUSEBUTTONDOWN && (e.button.button == SDL_BUTTON_RIGHT ||
                                            (pointerHeld && (SDL_GetModState() & KMOD_CTRL)))) {
        libraryRightClick = true;
        pointerHeld = false;
      }
      if (pointerHeld) {
        pointerPulse = 1; // a click the core sees even when press and release fall in one frame
        libraryClick = true;
      }
    }
#ifndef _WIN32
    // The cursor shows while a real mouse is used (not a touch screen's mouse events) and goes at the next key / button.
    if ((e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) ||
        ((e.type == SDL_MOUSEBUTTONDOWN) && e.button.which != SDL_TOUCH_MOUSEID))
      SDL_ShowCursor(SDL_ENABLE);
    else if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_JOYBUTTONDOWN)
      SDL_ShowCursor(SDL_DISABLE);
#endif
    if (e.type == SDL_MOUSEMOTION) {
      mouseWindowX = e.motion.x, mouseWindowY = e.motion.y;
      pointer_at(e.motion.x, e.motion.y);
    }
    if ((e.type == SDL_DROPFILE || e.type == SDL_DROPTEXT) && e.drop.file) {
      snprintf(libraryDropped, sizeof libraryDropped, "%s", e.drop.file);
      SDL_free(e.drop.file);
    }
    if (e.type == SDL_MOUSEWHEEL) {
      int steps = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
      wheelSteps += steps;
      libraryWheel += steps;
    }
    if (e.type == SDL_USEREVENT) // the menu bar
      command(e.user.code);
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
      windowAway = false;
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
      windowAway = true;
      inputReset = true;
      held = pressed = 0;
      pointerHeld = false; // its button-up may go to the window that took the focus
      pointerPulse = 0;
      pointerDown = false;
      libraryClick = libraryRightClick = false;
      libraryWheel = 0;
    }
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
      outputChanged = true;
  }
  script_mouse();
  script_resize();
  script_keys();
}
/* POCKET_MOUSE="TICK:X:Y[:c],...": where the cursor is (window pixels) from that tick on, ',c' clicking then, for
 * tests; the same tick numbering as POCKET_KEYS. */
static struct {
  unsigned at;
  int x, y, wheel;
  bool click;
} mscript[16];
static int mscriptN = -1;
static void script_mouse(void) {
  if (mscriptN < 0) {
    mscriptN = 0;
    char buf[512], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_MOUSE") ? getenv("POCKET_MOUSE") : "");
    for (char *t = strtok_r(buf, ",", &save); t && mscriptN < 16; t = strtok_r(NULL, ",", &save)) {
      unsigned at;
      int x, y;
      char c = 0;
      if (sscanf(t, "%u:%d:%d:%c", &at, &x, &y, &c) >= 3)
        mscript[mscriptN] = (typeof(mscript[0])){at, x, y, c == 'd' ? -1 : c == 'u' ? 1 : 0, c == 'c'}, mscriptN++;
    }
  }
  for (int i = 0; i < mscriptN; i++)
    if (tick == mscript[i].at) {
      mouseWindowX = mscript[i].x, mouseWindowY = mscript[i].y;
      pointer_at(mscript[i].x, mscript[i].y);
      if (mscript[i].click) {
        pointerPulse = 1;
        libraryClick = true;
      }
      wheelSteps += mscript[i].wheel;
      libraryWheel += mscript[i].wheel;
    }
}
/* POCKET_RESIZE="TICK:WxH,...": resizes the window at that tick, as a dragged edge or fullscreen would, for tests:
 * the core lays its canvas out for the new panel. */
static struct {
  unsigned at;
  int w, h;
} rscript[8];
static int rscriptN = -1;
static void script_resize(void) {
  if (rscriptN < 0) {
    rscriptN = 0;
    char buf[128], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_RESIZE") ? getenv("POCKET_RESIZE") : "");
    for (char *t = strtok_r(buf, ",", &save); t && rscriptN < 8; t = strtok_r(NULL, ",", &save)) {
      unsigned at;
      int w, h;
      if (sscanf(t, "%u:%dx%d", &at, &w, &h) == 3 && w > 0 && h > 0)
        rscript[rscriptN++] = (typeof(rscript[0])){at, w, h};
    }
  }
  for (int i = 0; i < rscriptN; i++)
    if (tick == rscript[i].at && window) {
      SDL_SetWindowSize(window, rscript[i].w, rscript[i].h);
      outputChanged = true;
    }
}
static void repeat_directions(void) {
  for (int k = RIGHT; k <= DOWN; k++)
    if ((held & (1u << k)) && SDL_GetTicks() >= repeatAt[k]) {
      pressed |= 1u << k;
      repeatAt[k] = SDL_GetTicks() + 90;
    }
}
static bool down(int k) { return (pressed & (1u << k)) != 0; }

/* Hotkeys: a button per action, set on the host menu's HOTKEYS page (host.ini hk_*). FF and rewind act while held,
 * the states on the press. A hotkey's button never reaches the game. The keyboard keeps its own (Space, R, F2, F4). */
enum { HK_FF, HK_REWIND, HK_SAVE, HK_LOAD, HK_ACTIONS };
static const char *const hotkeyIds[HK_ACTIONS] = {"ff", "rewind", "savestate", "loadstate"};
static unsigned hotkeyMask;
// The game's own buttons and the menu's stay theirs: A, B, START, SELECT, the D-pad (and the stick's), MENU.
static bool hotkey_assignable(int k) {
  return k == L || k == R || k == X || k == Y || k == L2 || k == R2 || k == L3 || k == R3 || (k >= P1 && k <= P4);
}
// START+SELECT opens the host menu too, unless POCKET_MENU_COMBO=off (PortMaster: there it is the port's exit).
static bool comboMenu = true;
static const struct {
  const char *name;
  int key;
} keyNames[] = {{"a", A},       {"b", B},         {"x", X},         {"y", Y},           {"l1", L},
                {"r1", R},      {"l2", L2},       {"r2", R2},       {"up", UP},         {"down", DOWN},
                {"left", LEFT}, {"right", RIGHT}, {"menu", MENU},   {"select", SELECT}, {"start", START}, {"l3", L3},
                {"sup", SUP},   {"sdown", SDOWN}, {"sleft", SLEFT}, {"sright", SRIGHT}, {"r3", R3},
                {"p1", P1},     {"p2", P2},       {"p3", P3},       {"p4", P4}};
static int key_named(const char *s) {
  for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
    if (!strcmp(s, keyNames[i].name))
      return keyNames[i].key;
  return -1;
}
static const char *key_name(int k) {
  for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
    if (keyNames[i].key == k)
      return keyNames[i].name;
  return "off";
}
/* ---- the host's settings (DATA/host.ini): what its menu sets ---- */
static struct {
  int ffSpeed; // 0 = MAX (as many frames as fit), else 2..8
  bool rewind, lcd;
  bool swapAB; // the controller's A / B and X / Y the other way round (devices disagree on which is SDL's "A")
  int hotkey[HK_ACTIONS]; // the HOTKEYS page's button per action, -1 none
  int dsHotkey[HK_ACTIONS];
  int modifier;
  bool fullscreen; // the WINDOW setting
  int panel;       // the PANEL setting: 0 = the window's size, else a preset the core lays out for
  int winX, winY, winW, winH; // where the window was at the last quit (winW 0: never saved)
  bool resume;    // CONTINUE ON OPEN: a game opens where it was left (its state at the last quit)
  bool pauseAway; // PAUSE IN BACKGROUND: the game waits while another window has the focus
  char savesFolder[PATH_MAX]; // SAVES FOLDER: "" = DATA/saves, else that folder (iCloud Drive, Google Drive, ...)
  bool dsSafe; // DS ENGINE: SAFE runs DSperate's interpreter (dsperate_jit disabled), FAST its recompiler
} hostSet = {0, false, false, false, {R2, L2, -1, -1}, {R2, L2, -1, -1}, SELECT, false, 0, 0, 0, 0, 0, true, true, "", false};
static char hostIni[PATH_MAX];
/* A DS game's L2 / R2 are its companion and screen buttons: with no modifier held for them, its FAST FORWARD and REWIND
 * go back to the stick clicks (R3 / L3), and come back to R2 / L2 when a modifier is picked again. */
static void ds_hotkeys_follow_modifier(void) {
  int *ds = hostSet.dsHotkey;
  if (hostSet.modifier < 0 && ds[HK_FF] == R2 && ds[HK_REWIND] == L2) {
    ds[HK_FF] = R3;
    ds[HK_REWIND] = L3;
  } else if (hostSet.modifier >= 0 && ds[HK_FF] == R3 && ds[HK_REWIND] == L3) {
    ds[HK_FF] = R2;
    ds[HK_REWIND] = L2;
  }
}
/* A new modifier can't also be a hotkey's own button (that hotkey would never fire): the hotkey lets it go. */
static void modifier_takes(int k) {
  for (int i = 0; i < HK_ACTIONS; i++) {
    if (hostSet.hotkey[i] == k)
      hostSet.hotkey[i] = -1;
    if (hostSet.dsHotkey[i] == k)
      hostSet.dsHotkey[i] = -1;
  }
}
static void host_load(void) {
  FILE *f = fopen(hostIni, "r");
  char line[PATH_MAX + 32];
  int v;
  bool modifierSaved = false;
  while (f && fgets(line, sizeof line, f)) {
    if (sscanf(line, "ff_speed=%d", &v) == 1)
      hostSet.ffSpeed = v == 0 || (v >= 2 && v <= 8) ? v : 0;
    if (sscanf(line, "rewind=%d", &v) == 1)
      hostSet.rewind = v != 0;
    if (sscanf(line, "lcd=%d", &v) == 1)
      hostSet.lcd = v != 0;
    if (sscanf(line, "swap_ab=%d", &v) == 1)
      hostSet.swapAB = v != 0;
    if (sscanf(line, "fullscreen=%d", &v) == 1)
      hostSet.fullscreen = v != 0;
    if (sscanf(line, "panel=%d", &v) == 1 && v >= 0 && v < PD_PANEL_PRESETS)
      hostSet.panel = v;
    if (sscanf(line, "resume=%d", &v) == 1)
      hostSet.resume = v != 0;
    if (sscanf(line, "pause_away=%d", &v) == 1)
      hostSet.pauseAway = v != 0;
    if (sscanf(line, "ds_safe=%d", &v) == 1)
      hostSet.dsSafe = v != 0;
    if (!strncmp(line, "saves_dir=", 10)) {
      snprintf(hostSet.savesFolder, sizeof hostSet.savesFolder, "%s", line + 10);
      hostSet.savesFolder[strcspn(hostSet.savesFolder, "\r\n")] = 0;
    }
    int wx, wy, ww, wh;
    if (sscanf(line, "window=%d,%d,%d,%d", &wx, &wy, &ww, &wh) == 4 && ww >= 160 && wh >= 144 && ww <= 16384 && wh <= 16384)
      hostSet.winX = wx, hostSet.winY = wy, hostSet.winW = ww, hostSet.winH = wh;
    char modifier[16];
    if (sscanf(line, "hk_modifier=%15s", modifier) == 1) {
      int k = key_named(modifier);
      if (k == SELECT || k == START || hotkey_assignable(k) || !strcmp(modifier, "off")) {
        hostSet.modifier = k;
        modifierSaved = true;
      }
    }
    for (int i = 0; i < HK_ACTIONS; i++) {
      char key[32], name[16];
      snprintf(key, sizeof key, "hk_%s=%%15s", hotkeyIds[i]);
      if (sscanf(line, key, name) == 1)
        hostSet.hotkey[i] = hotkey_assignable(key_named(name)) ? key_named(name) : -1;
      snprintf(key, sizeof key, "ds_hk_%s=%%15s", hotkeyIds[i]);
      if (sscanf(line, key, name) == 1)
        hostSet.dsHotkey[i] = hotkey_assignable(key_named(name)) ? key_named(name) : -1;
    }
    for (int i = 0; i < KB_COUNT; i++) {
      char key[32];
      snprintf(key, sizeof key, "kb_%s=%%d", kbIds[i]);
      if (sscanf(line, key, &v) == 1 && v >= 0)
        kbKeys[i] = v;
    }
  }
  if (f)
    fclose(f);
  if (!modifierSaved && hostSet.dsHotkey[HK_FF] == R3 && hostSet.dsHotkey[HK_REWIND] == L3) {
    hostSet.dsHotkey[HK_FF] = R2;
    hostSet.dsHotkey[HK_REWIND] = L2;
  }
  ds_hotkeys_follow_modifier();
  swapFace = hostSet.swapAB;
}
static void host_write(void) {
  FILE *f = fopen(hostIni, "w");
  if (!f)
    return;
  fprintf(f, "ff_speed=%d\nrewind=%d\nlcd=%d\nswap_ab=%d\n", hostSet.ffSpeed, hostSet.rewind, hostSet.lcd, hostSet.swapAB);
  fprintf(f, "fullscreen=%d\npanel=%d\n", hostSet.fullscreen, hostSet.panel);
  fprintf(f, "resume=%d\npause_away=%d\nsaves_dir=%s\nds_safe=%d\n", hostSet.resume, hostSet.pauseAway, hostSet.savesFolder,
          hostSet.dsSafe);
  if (hostSet.winW)
    fprintf(f, "window=%d,%d,%d,%d\n", hostSet.winX, hostSet.winY, hostSet.winW, hostSet.winH);
  fprintf(f, "hk_modifier=%s\n", key_name(hostSet.modifier));
  for (int i = 0; i < HK_ACTIONS; i++) {
    fprintf(f, "hk_%s=%s\n", hotkeyIds[i], key_name(hostSet.hotkey[i]));
    fprintf(f, "ds_hk_%s=%s\n", hotkeyIds[i], key_name(hostSet.dsHotkey[i]));
  }
  for (int i = 0; i < KB_COUNT; i++)
    fprintf(f, "kb_%s=%d\n", kbIds[i], kbKeys[i]);
  fclose(f);
}
/* ---- a game's own settings (the game list's GAME SETTINGS, DATA/game-settings/<file>.ini): LCD FILTER, FF SPEED,
 * REWIND and A/B BUTTONS for that game only, -1 = as for every game. While it plays they are hostSet's; a change made
 * in its menu to one of them stays the game's, the others are everyone's. ---- */
enum { GS_LCD, GS_FF, GS_REWIND, GS_SWAP, GS_COUNT };
static const char *const gsIds[GS_COUNT] = {"lcd", "ff_speed", "rewind", "swap_ab"};
static struct {
  bool on;              // a game with its own settings is playing
  char file[PATH_MAX * 2];
  int own[GS_COUNT];    // the game's, -1 = everyone's
  int every[GS_COUNT];  // everyone's, while the game's are in hostSet
} gameSet;
static int gs_get(int i) {
  return i == GS_LCD ? hostSet.lcd : i == GS_FF ? hostSet.ffSpeed : i == GS_REWIND ? hostSet.rewind : hostSet.swapAB;
}
static void gs_set(int i, int v) {
  if (i == GS_LCD)
    hostSet.lcd = v != 0;
  else if (i == GS_FF)
    hostSet.ffSpeed = v == 0 || (v >= 2 && v <= 8) ? v : 0;
  else if (i == GS_REWIND)
    hostSet.rewind = v != 0;
  else
    hostSet.swapAB = swapFace = v != 0;
}
static void gs_path(const char *game, char *out, size_t size) {
  const char *slash = strrchr(game, '/');
  snprintf(out, size, "%s/game-settings/%s.ini", root, slash ? slash + 1 : game);
}
static void gs_read(const char *game, int *own) {
  char path[PATH_MAX * 3], line[128];
  gs_path(game, path, sizeof path);
  for (int i = 0; i < GS_COUNT; i++)
    own[i] = -1;
  FILE *f = fopen(path, "r");
  while (f && fgets(line, sizeof line, f))
    for (int i = 0; i < GS_COUNT; i++) {
      char key[32];
      int v;
      snprintf(key, sizeof key, "%s=%%d", gsIds[i]);
      if (sscanf(line, key, &v) == 1 && v >= -1 && v <= 8)
        own[i] = v;
    }
  if (f)
    fclose(f);
}
static void gs_write(const char *game, const int *own) {
  char path[PATH_MAX * 3], dir[PATH_MAX + 16];
  gs_path(game, path, sizeof path);
  bool any = false;
  for (int i = 0; i < GS_COUNT; i++)
    any |= own[i] >= 0;
  if (!any) { // nothing of its own: no file
    remove(path);
    return;
  }
  snprintf(dir, sizeof dir, "%s/game-settings", root);
  pd_mkdir(dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (int i = 0; i < GS_COUNT; i++)
    if (own[i] >= 0)
      fprintf(f, "%s=%d\n", gsIds[i], own[i]);
  fclose(f);
}
static void host_save(void) {
  if (!gameSet.on) {
    host_write();
    return;
  }
  // The game's own rows go to its file, everyone's to host.ini.
  int now[GS_COUNT];
  for (int i = 0; i < GS_COUNT; i++) {
    now[i] = gs_get(i);
    if (gameSet.own[i] >= 0)
      gameSet.own[i] = now[i];
    else
      gameSet.every[i] = now[i];
  }
  gs_write(gameSet.file, gameSet.own);
  for (int i = 0; i < GS_COUNT; i++)
    gs_set(i, gameSet.every[i]);
  host_write();
  for (int i = 0; i < GS_COUNT; i++)
    gs_set(i, now[i]);
}
static void game_settings_begin(const char *game) {
  gs_read(game, gameSet.own);
  snprintf(gameSet.file, sizeof gameSet.file, "%s", game);
  for (int i = 0; i < GS_COUNT; i++) {
    gameSet.every[i] = gs_get(i);
    if (gameSet.own[i] >= 0)
      gs_set(i, gameSet.own[i]);
  }
  gameSet.on = true;
}
static void game_settings_end(void) {
  if (!gameSet.on)
    return;
  for (int i = 0; i < GS_COUNT; i++)
    gs_set(i, gameSet.every[i]);
  gameSet.on = false;
}
// A hotkey's button as the HOTKEYS page shows it.
static const char *hotkey_label(int k) {
  static const char *const labels[] = {[START] = "START", [SELECT] = "SELECT", [L] = "L1", [R] = "R1",  [X] = "X",         [Y] = "Y",         [L2] = "L2",
                                       [R2] = "R2", [L3] = "L3", [R3] = "R3",       [P1] = "PADDLE 1", [P2] = "PADDLE 2",
                                       [P3] = "PADDLE 3", [P4] = "PADDLE 4"};
  return k >= 0 && k < (int)(sizeof labels / sizeof labels[0]) && labels[k] ? labels[k] : "OFF";
}
/* The actions held this frame; *fired: those whose button went down now. The keyboard's own keys count too. */
static unsigned hotkeys_held(const int *hotkeys, unsigned modifier, unsigned *latched, unsigned *fired) {
  unsigned on = 0;
  *fired = 0;
  hotkeyMask = 0;
  for (int i = 0; i < HK_ACTIONS; i++) {
    int k = hotkeys[i];
    // REWIND's button is the game's while REWIND is off (L2 to start with: the core's own buttons may be on it).
    if (k < 0 || (i == HK_REWIND && !hostSet.rewind))
      continue;
    unsigned bit = 1u << k;
    if (bit == modifier)
      continue;
    if (modifier) {
      if ((held & modifier) && (pressed & bit))
        *latched |= bit;
      if (!(*latched & bit) || !(held & modifier))
        continue;
    }
    hotkeyMask |= bit;
    // pressed too: a tap shorter than a frame is let go before this sees it.
    if (!((held | pressed) & bit))
      continue;
    on |= 1u << i;
    if (pressed & bit)
      *fired |= 1u << i;
  }
  if (held & (1u << FFKEY))
    on |= 1u << HK_FF;
  if (held & (1u << RWKEY))
    on |= 1u << HK_REWIND;
  if (down(SAVEKEY))
    *fired |= 1u << HK_SAVE;
  if (down(LOADKEY))
    *fired |= 1u << HK_LOAD;
  return on;
}

/* ---- files ---- */
static bool exists(const char *p) { return access(p, F_OK) == 0; }
static unsigned char *read_file(const char *path, size_t *size, size_t max) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  unsigned char *d = len > 0 && (size_t)len <= max ? malloc((size_t)len) : NULL;
  if (d && fread(d, 1, (size_t)len, f) != (size_t)len) {
    free(d);
    d = NULL;
  }
  fclose(f);
  *size = d ? (size_t)len : 0;
  return d;
}
// Written beside, then renamed over: a crash leaves the old file or the new one, never half of one.
static bool write_file(const char *path, const void *d, size_t n, bool sync) {
  char tmp[PATH_MAX + 8];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  bool ok = fwrite(d, 1, n, f) == n && fflush(f) == 0 && (!sync || fsync(fileno(f)) == 0);
  ok = fclose(f) == 0 && ok;
  if (ok && pd_rename(tmp, path) == 0)
    return true;
  unlink(tmp);
  return false;
}
/* A copy of the battery save before the game opens, like Android's and as host_io.c's pocket_save_backup names them (the
 * DS path's, and the hosts' before this one): DIR/STEM.backup-YYYYmmdd-HHMMSS.EXT, a new one only when the save data
 * differs from the newest backup's, the newest 10 kept. Every save chip is a multiple of 512 bytes, so what is past the
 * last whole 512 is mGBA's clock footer, which moves every session: a save where only the clock moved is the same save.
 * A save that can't be backed up keeps the game closed. */
static bool same_save(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
  size_t n = an - an % 512;
  if (!n || n != bn - bn % 512)
    return an == bn && !memcmp(a, b, an);
  return !memcmp(a, b, n);
}
static int newest_first(const void *a, const void *b) { return -strcmp(*(char *const *)a, *(char *const *)b); }
static bool save_backup(const char *sav, const char *dir) {
  size_t n;
  unsigned char *d = read_file(sav, &n, 64u << 20);
  if (!d)
    return !exists(sav); // nothing to keep yet
  if (!n) {
    free(d);
    return true;
  }
  pd_mkdir(dir);
  const char *file = strrchr(sav, '/');
  file = file ? file + 1 : sav;
  const char *dot = strrchr(file, '.');
  char stem[512], ext[32] = "sav";
  snprintf(stem, sizeof stem, "%.*s.backup-", dot && dot != file ? (int)(dot - file) : (int)strlen(file), file);
  if (dot && dot != file)
    snprintf(ext, sizeof ext, "%s", dot + 1);
  char *list[256];
  int count = 0;
  DIR *dd = opendir(dir);
  for (struct dirent *e; dd && (e = readdir(dd)) && count < 256;)
    if (!strncmp(e->d_name, stem, strlen(stem)) && !strstr(e->d_name, ".tmp"))
      list[count++] = strdup(e->d_name);
  if (dd)
    closedir(dd);
  qsort(list, count, sizeof *list, newest_first);
  bool write = true, ok = true;
  char path[PATH_MAX * 4 + 64];
  if (count) {
    size_t m;
    snprintf(path, sizeof path, "%s/%s", dir, list[0]);
    unsigned char *last = read_file(path, &m, 64u << 20);
    write = !last || !same_save(last, m, d, n);
    free(last);
  }
  if (write) {
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof path, "%s/%s%s.%s", dir, stem, stamp, ext);
    ok = write_file(path, d, n, true);
  }
  // The newest 10 stay (a new one counts as the newest).
  for (int i = ok && write ? 9 : 10; ok && i < count; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, list[i]);
    unlink(path);
  }
  for (int i = 0; i < count; i++)
    free(list[i]);
  free(d);
  return ok;
}

/* ---- the core ---- */
static struct {
  void *dl;
  void (*init)(void);
  void (*deinit)(void);
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*get_system_info)(struct retro_system_info *);
  void (*get_system_av_info)(struct retro_system_av_info *);
  bool (*load_game)(const struct retro_game_info *);
  void (*unload_game)(void);
  void (*run)(void);
  size_t (*serialize_size)(void);
  bool (*serialize)(void *, size_t);
  bool (*unserialize)(const void *, size_t);
  void *(*get_memory_data)(unsigned);
  size_t (*get_memory_size)(unsigned);
} core;
static bool core_open(const char *path) {
  core.dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!core.dl) {
    fprintf(stderr, "%s\n", dlerror());
    return false;
  }
#define SYM(field, name)                                                                                       \
  if (!(*(void **)&core.field = dlsym(core.dl, name))) {                                                       \
    fprintf(stderr, "%s: no %s\n", path, name);                                                                \
    dlclose(core.dl);                                                                                          \
    core.dl = NULL;                                                                                            \
    return false;                                                                                              \
  }
  SYM(init, "retro_init")
  SYM(deinit, "retro_deinit")
  SYM(set_environment, "retro_set_environment")
  SYM(set_video_refresh, "retro_set_video_refresh")
  SYM(set_audio_sample, "retro_set_audio_sample")
  SYM(set_audio_sample_batch, "retro_set_audio_sample_batch")
  SYM(set_input_poll, "retro_set_input_poll")
  SYM(set_input_state, "retro_set_input_state")
  SYM(get_system_info, "retro_get_system_info")
  SYM(get_system_av_info, "retro_get_system_av_info")
  SYM(load_game, "retro_load_game")
  SYM(unload_game, "retro_unload_game")
  SYM(run, "retro_run")
  SYM(serialize_size, "retro_serialize_size")
  SYM(serialize, "retro_serialize")
  SYM(unserialize, "retro_unserialize")
  SYM(get_memory_data, "retro_get_memory_data")
  SYM(get_memory_size, "retro_get_memory_size")
#undef SYM
  return true;
}

/* The core's options: their defaults as it declares them, and the panel this host has. */
static struct {
  const char *key, *value;
  char legacyDefault[128];
} options[160];
static int optionN;
static char panelValue[32];
static char windowValue[32];   // the window the picture is drawn in, whatever PANEL preset is held
static bool optionsChanged;
static void options_from(const struct retro_core_option_v2_definition *d) {
  for (optionN = 0; d && d->key && optionN < (int)(sizeof options / sizeof options[0]); d++, optionN++) {
    options[optionN].key = d->key;
    options[optionN].value = d->default_value;
  }
}
static void options_from_v1(const struct retro_core_option_definition *d) {
  for (optionN = 0; d && d->key && optionN < (int)(sizeof options / sizeof options[0]); d++, optionN++) {
    options[optionN].key = d->key;
    options[optionN].value = d->default_value;
  }
}

static void options_from_variables(const struct retro_variable *v) {
  for (optionN = 0; v && v->key && optionN < (int)(sizeof options / sizeof options[0]); v++) {
    const char *first = v->value ? strchr(v->value, ';') : NULL;
    if (!first)
      continue;
    first++;
    first += strspn(first, " ");
    size_t len = strcspn(first, "|");
    if (len >= sizeof options[0].legacyDefault)
      continue;
    options[optionN].key = v->key;
    memcpy(options[optionN].legacyDefault, first, len);
    options[optionN].legacyDefault[len] = '\0';
    options[optionN].value = options[optionN].legacyDefault;
    optionN++;
  }
}

static enum retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_system_av_info av;
static SDL_Texture *frameTexture;
static int textureW, textureH, frameW, frameH;
static enum retro_pixel_format textureFormat; // what frameTexture was made for: a core of another format needs a new one
static double srcRate = 32768, rateTrim = 1;   // the core's audio rate (SET_SYSTEM_AV_INFO can change it), the pacing's trim
static bool ffNow, rewindingNow, ffQuick; // ffQuick: the quick menu's FF toggle, until it is turned off again
static int ffSpeedNow = 1;
static void core_log(enum retro_log_level level, const char *fmt, ...) {
  if (level < RETRO_LOG_WARN && !getenv("POCKET_CORE_LOG"))
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
static void frame_texture(void) {
  int w = (int)av.geometry.max_width, h = (int)av.geometry.max_height;
  if (frameTexture && w <= textureW && h <= textureH && pixelFormat == textureFormat)
    return;
  if (frameTexture)
    SDL_DestroyTexture(frameTexture);
  frameTexture = SDL_CreateTexture(renderer,
                                   pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888
                                   : pixelFormat == RETRO_PIXEL_FORMAT_RGB565 ? SDL_PIXELFORMAT_RGB565
                                                                              : SDL_PIXELFORMAT_ARGB1555,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
  textureW = w;
  textureH = h;
  textureFormat = pixelFormat;
  if (frameTexture) {
    SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_NONE);
    // Pixel art stays pixel art: the picture and the core's touch panel both scale nearest, so a fractional window size
    // never blurs them. The hint covers backends that ignore the per-texture mode; the mode covers the rest.
    SDL_SetTextureScaleMode(frameTexture, SDL_ScaleModeNearest);
  } else
    fprintf(stderr, "Texture: %s\n", SDL_GetError());
}
static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = NULL;
    if (!strcmp(v->key, "pokedaisy_ds_select_modifier"))
      v->value = hostSet.modifier == SELECT ? "enabled" : "disabled";
    if (!strcmp(v->key, "pokedaisy_panel"))
      v->value = panelValue;
    if (!strcmp(v->key, "pokedaisy_window"))
      v->value = windowValue;
    // ASPECT is the companion's own (the app's SETTINGS > SCREEN): auto leaves its file's setting be.
    if (!strcmp(v->key, "pokedaisy_aspect"))
      v->value = "auto";
    // DS ENGINE: SAFE is the interpreter; FAST leaves DSperate's own default (its recompiler).
    if (!strcmp(v->key, "dsperate_jit") && hostSet.dsSafe)
      v->value = "disabled";
    for (int i = 0; i < optionN && !v->value; i++)
      if (!strcmp(options[i].key, v->key))
        v->value = options[i].value;
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = optionsChanged;
    optionsChanged = false;
    return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *(unsigned *)data = 2;
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    options_from(data ? ((const struct retro_core_options_v2 *)data)->definitions : NULL);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
    const struct retro_core_options_v2_intl *i = data;
    options_from(i && i->us ? i->us->definitions : NULL);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    options_from_v1(data);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    options_from_v1(data ? ((const struct retro_core_options_intl *)data)->us : NULL);
    return true;
  case RETRO_ENVIRONMENT_SET_VARIABLES:
    options_from_variables(data);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
    enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
    if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_0RGB1555)
      return false;
    pixelFormat = f;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char **)data = savesDir;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *(const char **)data = root;
    return true;
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
    av.geometry = *(const struct retro_game_geometry *)data;
    if (renderer)
      frame_texture();
    return true;
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    av = *(const struct retro_system_av_info *)data;
    if (av.timing.sample_rate > 0)
      srcRate = av.timing.sample_rate; // the audio resampler follows the core's new rate
    if (renderer)
      frame_texture();
    return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    *(bool *)data = true;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback *)data)->log = core_log;
    return true;
  case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
    if (data)
      *(int *)data = 3;
    return true;
  case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
    *(bool *)data = ffNow;
    return true;
  case PD_ENV_SET_FAST_FORWARD:
    ffQuick = data && *(const bool *)data;
    return true;
  case PD_ENV_SET_TOUCH_PANEL: {
    const struct pd_touch_panel *p = data;
    panelPending = p && p->active && p->pixels && p->src_w > 0 && p->src_h > 0 && p->w > 0 && p->h > 0;
    if (panelPending)
      panelMsg = *p;
    return true;
  }
  case PD_ENV_SET_HUD: {
    const struct pd_hud *h = data;
    hudPending = h && h->active && h->pixels && h->src_w > 0 && h->src_h > 0 && h->w > 0 && h->h > 0;
    hudHave = true;
    if (hudPending)
      hudMsg = *h;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
    struct retro_throttle_state *t = data;
    t->mode = rewindingNow ? RETRO_THROTTLE_REWINDING
              : ffNow      ? RETRO_THROTTLE_FAST_FORWARD
                           : RETRO_THROTTLE_VSYNC;
    t->rate = (float)(av.timing.fps * (ffNow ? ffSpeedNow : 1));
    return true;
  }
  default:
    return false;
  }
}

/* ---- video ---- */
static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
  panelActive = panelPending;
  panelPending = false;
  if (hudHave) // a frame the core only repeats keeps the HUD where it was
    hudActive = hudPending;
  hudPending = hudHave = false;
  if (!data || !frameTexture) // a dupe: the last frame stays
    return;
  SDL_Rect r = {0, 0, (int)w, (int)h};
  SDL_UpdateTexture(frameTexture, &r, data, (int)pitch);
  if ((int)w != frameW || (int)h != frameH) // the core's canvas: it follows the aspect and the panel
    fprintf(stderr, "Frame: %ux%u\n", w, h);
  frameW = (int)w;
  frameH = (int)h;
}
/* The frame centred, at the largest scale the screen holds. The core sizes its canvas to the panel, so this is a whole
 * scale normally; where a whole one would leave the picture short of the screen (a canvas whose aspect isn't the
 * window's, or an odd panel) it fits instead, so the picture fills the screen rather than sitting in bars. */
static SDL_Rect frame_place(int ow, int oh) {
  SDL_Rect r = {0, 0, ow, oh};
  if (frameW <= 0 || frameH <= 0)
    return r;
  double fit = ow / (double)frameW < oh / (double)frameH ? ow / (double)frameW : oh / (double)frameH;
  int whole = (int)fit;
  double s = whole >= 1 && whole / fit >= 0.98 ? whole : fit; // whole pixels only when they fill the screen
  r.w = (int)(frameW * s + 0.5);
  r.h = (int)(frameH * s + 0.5);
  if (s == whole) {
    // A whole scale a pixel or three short of the screen would show as a thin bar down one side: cover it instead (the
    // stretch is a fraction of a pixel in a texel, invisible next to the bar).
    if (r.w < ow && ow - r.w <= 4)
      r.w = ow;
    if (r.h < oh && oh - r.h <= 4)
      r.h = oh;
  }
  r.x = (ow - r.w) / 2;
  r.y = (oh - r.h) / 2;
  return r;
}

/* The overlay (notices, the host menu, the game list): a canvas at the largest whole scale that keeps it at least
 * 320x240 (as the companion's), KEY_COLOR see-through. */
static uint32_t overlayPx[OVERLAY_MAX_W * OVERLAY_MAX_H];
static struct pd_canvas overlay;
static SDL_Texture *overlayTexture;
static int overlayTexW, overlayTexH;
static void overlay_begin(void) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  int s = ow / 320 < oh / 240 ? ow / 320 : oh / 240;
  if (s < 1)
    s = 1;
  int w = ow / s, h = oh / s;
  w = w > OVERLAY_MAX_W ? OVERLAY_MAX_W : w;
  h = h > OVERLAY_MAX_H ? OVERLAY_MAX_H : h;
  pd_canvas_init(&overlay, w, h, overlayPx);
  pd_fill(&overlay, 0, 0, w, h, KEY_COLOR);
}
static void overlay_draw(bool opaque) {
  if (!overlayTexture || overlayTexW != overlay.w || overlayTexH != overlay.h) {
    if (overlayTexture)
      SDL_DestroyTexture(overlayTexture);
    overlayTexture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, overlay.w, overlay.h);
    overlayTexW = overlay.w;
    overlayTexH = overlay.h;
    if (!overlayTexture)
      return;
    SDL_SetTextureBlendMode(overlayTexture, SDL_BLENDMODE_BLEND);
  }
  int n = overlay.w * overlay.h;
  for (int i = 0; i < n; i++) {
    uint32_t p = overlayPx[i] & 0xFFFFFF;
    overlayPx[i] = p == KEY_COLOR && !opaque ? 0 : 0xFF000000u | p;
  }
  SDL_UpdateTexture(overlayTexture, NULL, overlayPx, overlay.w * 4);
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  int s = ow / overlay.w < oh / overlay.h ? ow / overlay.w : oh / overlay.h;
  if (s < 1)
    s = 1;
  SDL_Rect r = {(ow - overlay.w * s) / 2, (oh - overlay.h * s) / 2, overlay.w * s, overlay.h * s};
  SDL_RenderCopy(renderer, overlayTexture, NULL, &r);
}
static bool overlay_notice(void) {
  if (SDL_GetTicks() >= messageUntil)
    return false;
  pd_fill(&overlay, 4, overlay.h - 30, overlay.w - 8, 24, 0x181818);
  pd_text_fit(&overlay, 10, overlay.h - 26, overlay.w - 20, message, 0xffffff, PD_NO_SHADOW);
  return true;
}

/* LCD (the host menu's LCD FILTER): Keegan Whitacre's output-space aperture mask (pokedaisy-brick dfc62a0),
 * multiplied over the whole presented frame. */
static SDL_Texture *lcdTexture;
static int lcdW, lcdH;
static void render_lcd(void) {
  if (!hostSet.lcd)
    return;
  int w, h;
  if (SDL_GetRendererOutputSize(renderer, &w, &h) < 0 || w <= 0 || h <= 0)
    return;
  if (!lcdTexture || lcdW != w || lcdH != h) {
    SDL_DestroyTexture(lcdTexture);
    lcdTexture = NULL;
    uint32_t *mask = malloc((size_t)w * h * 4);
    if (!mask)
      return;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        mask[y * w + x] = pocket_lcd_pixel(x, y);
    lcdTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (lcdTexture && (SDL_UpdateTexture(lcdTexture, NULL, mask, w * 4) < 0 ||
                       SDL_SetTextureBlendMode(lcdTexture, SDL_BLENDMODE_MOD) < 0)) {
      SDL_DestroyTexture(lcdTexture);
      lcdTexture = NULL;
    }
    free(mask);
    lcdW = w;
    lcdH = h;
  }
  if (lcdTexture)
    SDL_RenderCopy(renderer, lcdTexture, NULL, NULL);
}
/* A save-state slot's picture (SAVE STATE / LOAD STATE pages): drawn beside the slots. */
static SDL_Texture *slotThumb;
static bool slotThumbShow;
static int slotThumbW, slotThumbH;
static double presentMs;
static void present_game(const struct pd_menu *menu) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  SDL_RenderClear(renderer);
  frameRect = frame_place(ow, oh);
  if (frameTexture && frameW > 0) {
    SDL_Rect src = {0, 0, frameW, frameH};
    SDL_FRect dst = {(float)frameRect.x, (float)frameRect.y, (float)frameRect.w, (float)frameRect.h};
    SDL_RenderCopyF(renderer, frameTexture, &src, &dst);
  }
  // The core's touch panel: its full 256 x 192 bottom screen, scaled once, here, to the panel's place in the output.
  if (panelActive && panelMsg.pixels && frameW > 0 && frameH > 0) {
    if (!panelTexture || panelTexW != panelMsg.src_w || panelTexH != panelMsg.src_h) {
      if (panelTexture)
        SDL_DestroyTexture(panelTexture);
      panelTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        panelMsg.src_w, panelMsg.src_h);
      panelTexW = panelMsg.src_w;
      panelTexH = panelMsg.src_h;
      if (panelTexture) {
        SDL_SetTextureBlendMode(panelTexture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(panelTexture, SDL_ScaleModeNearest);
      }
    }
    if (panelTexture) {
      SDL_UpdateTexture(panelTexture, NULL, panelMsg.pixels, panelMsg.src_w * 4);
      float sx = (float)frameRect.w / (float)frameW, sy = (float)frameRect.h / (float)frameH;
      SDL_FRect dst = {(float)frameRect.x + panelMsg.x * sx, (float)frameRect.y + panelMsg.y * sy,
                       panelMsg.w * sx, panelMsg.h * sy};
      SDL_RenderCopyF(renderer, panelTexture, NULL, &dst);
    }
  }
  // The core's field HUD, from its own 256 x 192 picture: the frame's canvas is scaled to the window, and at a ZOOM step a
  // HUD blended into the canvas would be resampled with the picture. Drawn here it keeps the window's pixels at every step.
  if (hudActive && hudMsg.pixels && hudMsg.src_w > 0 && hudMsg.src_h > 0 && frameW > 0 && frameH > 0) {
    if (!hudTexture || hudTexW != hudMsg.src_w || hudTexH != hudMsg.src_h) {
      if (hudTexture)
        SDL_DestroyTexture(hudTexture);
      hudTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hudMsg.src_w,
                                     hudMsg.src_h);
      hudTexW = hudMsg.src_w;
      hudTexH = hudMsg.src_h;
      if (hudTexture) {
        SDL_SetTextureBlendMode(hudTexture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(hudTexture, SDL_ScaleModeNearest);
      }
    }
    if (hudTexture) {
      SDL_UpdateTexture(hudTexture, NULL, hudMsg.pixels, hudMsg.src_w * 4);
      const float sx = (float)frameRect.w / (float)frameW, sy = (float)frameRect.h / (float)frameH;
      const float bx = (float)frameRect.x + hudMsg.x * sx, by = (float)frameRect.y + hudMsg.y * sy;
      const float bw = hudMsg.w * sx, bh = hudMsg.h * sy;
      // SMALL / NORMAL: each quarter of the picture at its own corner of the box, a quarter of the box's size, as the core's
      // in-canvas blend places it. Drawn from the picture's own pixels in one go, a quarter is as sharp here as at 2x.
      if (hudMsg.pct >= 100) {
        SDL_FRect dst = {bx, by, bw, bh};
        SDL_RenderCopyF(renderer, hudTexture, NULL, &dst);
      } else {
        const float qw = bw * hudMsg.pct / 200.0f, qh = bh * hudMsg.pct / 200.0f;
        const float sw = (float)hudMsg.src_w / 2, sh = (float)hudMsg.src_h / 2;
        if (qw >= 1 && qh >= 1)
          for (int qy = 0; qy < 2; qy++)
            for (int qx = 0; qx < 2; qx++) {
              SDL_Rect src = {(int)(qx * sw), (int)(qy * sh), (int)sw, (int)sh};
              SDL_FRect dst = {qx ? bx + bw - qw : bx, qy ? by + bh - qh : by, qw, qh};
              SDL_RenderCopyF(renderer, hudTexture, &src, &dst);
            }
      }
    }
  }
  render_lcd();
  bool any = false;
  overlay_begin();
  if (menu) {
    pd_menu_draw((struct pd_menu *)menu, &overlay);
    any = true;
  }
  any |= overlay_notice();
  if (any)
    overlay_draw(false);
  if (menu && slotThumbShow && slotThumb && slotThumbW > 0) { // the slot's picture, at the right of its rows
    int tw = ow * 2 / 5, th = tw * slotThumbH / slotThumbW;
    if (th > oh / 2)
      th = oh / 2, tw = th * slotThumbW / slotThumbH;
    SDL_Rect border = {ow - tw - ow / 14 - 3, oh / 4 - 3, tw + 6, th + 6}, dst = {border.x + 3, border.y + 3, tw, th};
    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
    SDL_RenderFillRect(renderer, &border);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderCopy(renderer, slotThumb, NULL, &dst);
  }
  uint64_t t = SDL_GetPerformanceCounter();
  SDL_RenderPresent(renderer);
  presentMs += (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / SDL_GetPerformanceFrequency();
}
static bool capture(const char *path) {
  int w = W, h = H;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  uint32_t *px = malloc((size_t)w * h * 4);
  bool ok = px && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0;
  for (int i = 0; ok && i < w * h; i++)
    px[i] &= 0xFFFFFF;
  ok = ok && pd_png_write(path, w, h, px);
  free(px);
  return ok;
}

/* ---- audio: the core's samples resampled to the device, a little faster or slower to keep its queue near 60 ms
 * (the screen paces the game, not the sound card); fast-forward keeps every Nth sample, so it plays sped up. ---- */
static double resamplePos;
static int16_t prevL, prevR;
static unsigned audioSkip, audioKeepEvery = 1;
static bool muted;
static void audio_open(void) {
  SDL_AudioSpec want = {0};
  want.freq = OUT_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
  if (audio)
    SDL_PauseAudioDevice(audio, 0);
  else
    fprintf(stderr, "Audio unavailable: %s\n", SDL_GetError());
}
/* A handheld's ALSA device can go bad under the app (the screen turned off and on: "ALSA write failed
 * (unrecoverable)"), and SDL then closes it for good; it's reopened as soon as SDL says so, or within a second of it
 * no longer playing. */
static void audio_check(void) {
  static Uint32 next;
  bool stopped = audio && SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_STOPPED;
  if (!audioLost && !stopped)
    return;
  if (SDL_GetTicks() < next)
    return;
  next = SDL_GetTicks() + 1000;
  if (audio)
    SDL_CloseAudioDevice(audio);
  audio = 0;
  audio_open();
  audioLost = !audio;
  if (audio)
    fprintf(stderr, "Audio device reopened\n");
}
#ifdef POCKET_TEST_AUDIO
static unsigned long audioNonzero;
#endif
static size_t audio_batch(const int16_t *in, size_t frames) {
  static int16_t out[4096 * 2];
  if (!audio || muted)
    return frames;
  size_t n = 0;
  double step = srcRate * rateTrim / OUT_RATE;
  const bool room = SDL_GetQueuedAudioSize(audio) < OUT_RATE * 4 / 5; // never more than 200 ms behind
  for (size_t i = 0; i < frames; i++) {
    if (audioSkip++ % audioKeepEvery)
      continue;
    int16_t l = in[2 * i], r = in[2 * i + 1];
#ifdef POCKET_TEST_AUDIO
    audioNonzero += l || r;
#endif
    while (resamplePos < 1.0) {
      if (n == 4096) { // a batch longer than the buffer: send what's made and go on (never skip ahead in the input)
        if (room)
          SDL_QueueAudio(audio, out, (Uint32)(n * 4));
        n = 0;
      }
      out[2 * n] = (int16_t)(prevL + (l - prevL) * resamplePos);
      out[2 * n + 1] = (int16_t)(prevR + (r - prevR) * resamplePos);
      n++;
      resamplePos += step;
    }
    resamplePos -= 1.0;
    prevL = l;
    prevR = r;
  }
  if (n && room)
    SDL_QueueAudio(audio, out, (Uint32)(n * 4));
  return frames;
}
static void audio_sample(int16_t l, int16_t r) {
  int16_t s[2] = {l, r};
  audio_batch(s, 1);
}
static void audio_trim(double gameHz, double paceHz) {
  double rate = paceHz > 0 && gameHz > 0 ? paceHz / gameHz : 1;
  if (audio) {
    double queued = SDL_GetQueuedAudioSize(audio) / 4.0, target = OUT_RATE * 0.06;
    double d = (target - queued) / target;
    d = d > 1 ? 1 : d < -1 ? -1 : d;
    rate *= 1 - 0.005 * d; // short of the target: a little more output per input sample
  }
  rateTrim = rate;
}

/* ---- input to the core ---- */
static unsigned gameKeys; // this frame's joypad, in RETRO_DEVICE_ID_JOYPAD bits
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port)
    return 0;
  if (device == RETRO_DEVICE_JOYPAD)
    return id == RETRO_DEVICE_ID_JOYPAD_MASK ? (int16_t)gameKeys : (int16_t)((gameKeys >> id) & 1);
  if (device == RETRO_DEVICE_POINTER)
    return id == RETRO_DEVICE_ID_POINTER_X         ? (int16_t)pointerX
           : id == RETRO_DEVICE_ID_POINTER_Y       ? (int16_t)pointerY
           : id == RETRO_DEVICE_ID_POINTER_PRESSED ? pointerDown
                                                   : 0;
  if (device == RETRO_DEVICE_MOUSE)
    return id == RETRO_DEVICE_ID_MOUSE_WHEELUP     ? (int16_t)(wheelSteps > 0 ? wheelSteps : 0)
           : id == RETRO_DEVICE_ID_MOUSE_WHEELDOWN ? (int16_t)(wheelSteps < 0 ? -wheelSteps : 0)
                                                   : 0;
  return 0;
}
static unsigned joypad(unsigned keys) {
  static const struct {
    int key;
    unsigned id;
  } map[] = {{A, RETRO_DEVICE_ID_JOYPAD_A},           {B, RETRO_DEVICE_ID_JOYPAD_B},
             {SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT}, {START, RETRO_DEVICE_ID_JOYPAD_START},
             {RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},   {LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
             {UP, RETRO_DEVICE_ID_JOYPAD_UP},         {DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
             {R, RETRO_DEVICE_ID_JOYPAD_R},           {L, RETRO_DEVICE_ID_JOYPAD_L},
             {X, RETRO_DEVICE_ID_JOYPAD_X},           {Y, RETRO_DEVICE_ID_JOYPAD_Y},
             {L2, RETRO_DEVICE_ID_JOYPAD_L2},         {R2, RETRO_DEVICE_ID_JOYPAD_R2},
             {L3, RETRO_DEVICE_ID_JOYPAD_L3},         {R3, RETRO_DEVICE_ID_JOYPAD_R3}};
  unsigned bits = 0;
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    if (keys & (1u << map[i].key))
      bits |= 1u << map[i].id;
  return bits;
}

/* ---- rewind: a state every 2 frames, kept as the newest whole state plus the differences back from it (XOR,
 * zero runs packed), within a memory budget (POCKET_REWIND_MB, default 32): about 20 s on a GBA game. ---- */
#define REWIND_CAP 4096
static unsigned char *rewindNow, *rewindTmp;
static size_t rewindSize, rewindBytes, rewindBudget;
static struct {
  unsigned char *d;
  size_t n;
} rewindRing[REWIND_CAP];
static int rewindHead, rewindCount, rewindTick;
static void rewind_clear(void) {
  for (int i = 0; i < rewindCount; i++)
    free(rewindRing[(rewindHead + i) % REWIND_CAP].d);
  rewindHead = rewindCount = 0;
  rewindBytes = 0;
  free(rewindNow);
  free(rewindTmp);
  rewindNow = rewindTmp = NULL;
  rewindSize = 0;
}
// a ^ b packed: (u32 zeros, u32 literal count, literal bytes)...
static size_t delta_pack(const unsigned char *a, const unsigned char *b, size_t n, unsigned char *out) {
  size_t o = 0, i = 0;
  while (i < n) {
    size_t z = i;
    while (z < n && a[z] == b[z])
      z++;
    size_t lit = z;
    while (lit < n && (a[lit] != b[lit] || (lit + 1 < n && a[lit + 1] != b[lit + 1])))
      lit++;
    uint32_t zeros = (uint32_t)(z - i), count = (uint32_t)(lit - z);
    memcpy(out + o, &zeros, 4);
    memcpy(out + o + 4, &count, 4);
    o += 8;
    for (size_t k = z; k < lit; k++)
      out[o++] = a[k] ^ b[k];
    i = lit;
  }
  return o;
}
static void delta_apply(unsigned char *s, const unsigned char *d, size_t dn) {
  size_t i = 0, o = 0;
  while (o + 8 <= dn) {
    uint32_t zeros, count;
    memcpy(&zeros, d + o, 4);
    memcpy(&count, d + o + 4, 4);
    o += 8;
    i += zeros;
    for (uint32_t k = 0; k < count; k++)
      s[i++] ^= d[o++];
  }
}
static void rewind_push(void) {
  size_t size = core.serialize_size();
  if (!size)
    return;
  if (size != rewindSize) {
    rewind_clear();
    rewindNow = malloc(size);
    rewindTmp = malloc(size * 3 + 64); // the worst packing: 9 bytes per 3 (1 literal + its header)
    if (!rewindNow || !rewindTmp || !core.serialize(rewindNow, size)) {
      rewind_clear();
      return;
    }
    rewindSize = size;
    return;
  }
  static unsigned char *next;
  static size_t nextSize;
  if (nextSize != size) {
    free(next);
    next = malloc(size);
    nextSize = next ? size : 0;
  }
  if (!next || !core.serialize(next, size))
    return;
  size_t dn = delta_pack(rewindNow, next, size, rewindTmp);
  unsigned char *d = malloc(dn);
  if (!d)
    return;
  memcpy(d, rewindTmp, dn);
  while (rewindCount && (rewindCount == REWIND_CAP || rewindBytes + dn > rewindBudget)) {
    free(rewindRing[rewindHead].d);
    rewindBytes -= rewindRing[rewindHead].n;
    rewindHead = (rewindHead + 1) % REWIND_CAP;
    rewindCount--;
  }
  int at = (rewindHead + rewindCount++) % REWIND_CAP;
  rewindRing[at].d = d;
  rewindRing[at].n = dn;
  rewindBytes += dn;
  memcpy(rewindNow, next, size);
}
// One state back: false when there's none left.
static bool rewind_pop(void) {
  if (!rewindCount || !rewindNow)
    return false;
  int at = (rewindHead + --rewindCount) % REWIND_CAP;
  delta_apply(rewindNow, rewindRing[at].d, rewindRing[at].n);
  rewindBytes -= rewindRing[at].n;
  free(rewindRing[at].d);
  return core.unserialize(rewindNow, rewindSize);
}

/* ---- pacing: the screen's refresh when it's the game's (58-62 Hz and presents wait for it), else the timer ---- */
static bool vsyncPaced;
static int screenHz;
static void pacing_start(double gameHz) {
  SDL_DisplayMode mode = {0};
  screenHz = SDL_GetCurrentDisplayMode(0, &mode) == 0 ? mode.refresh_rate : 0;
  SDL_RendererInfo info;
  vsyncPaced = !getenv("POCKET_NO_VSYNC") && screenHz >= 58 && screenHz <= 62 &&
               SDL_GetRendererInfo(renderer, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC) && gameHz > 0;
}
// A refresh-paced loop whose presents don't wait (a driver ignoring vsync) goes back to the timer.
static void pacing_check(double frameMs) {
  static double sum;
  static int count;
  if (!vsyncPaced)
    return;
  sum += frameMs;
  if (++count < 120)
    return;
  if (sum / count < 12.0) {
    vsyncPaced = false;
    fprintf(stderr, "Presents don't wait for the refresh (%.1f ms apart): pacing by the timer\n", sum / count);
  }
  sum = 0;
  count = 0;
}
/* POCKET_PERF=1: every 5 s, how the frames went. */
static double emuMs;
static void perf_frame(uint64_t start, double hz) {
  static int on = -1, n, late;
  static double sum, worst, presents, emus;
  static uint64_t windowStart;
  if (on < 0)
    on = getenv("POCKET_PERF") != NULL;
  if (!on)
    return;
  uint64_t end = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
  double work = (double)(end - start) * 1000.0 / f;
  if (!windowStart)
    windowStart = end;
  n++;
  sum += work;
  presents += presentMs;
  emus += emuMs;
  presentMs = emuMs = 0;
  worst = work > worst ? work : worst;
  late += work > 1000.0 / hz;
  if (end - windowStart >= 5 * f) {
    fprintf(stderr,
            "perf: %d frames in %.1f s, core %.2f ms, work %.2f ms avg (%.2f in present) / %.2f worst, %d late, "
            "screen %d Hz, %s, audio queued %u B\n",
            n, (double)(end - windowStart) / f, emus / n, sum / n, presents / n, worst, late, screenHz,
            vsyncPaced ? "paced by the screen" : "paced by the timer", audio ? SDL_GetQueuedAudioSize(audio) : 0);
    n = late = 0;
    sum = worst = presents = emus = 0;
    windowStart = end;
  }
}

/* ---- a game ---- */
static void panel_follow(void) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  char v[32];
  const char *fixed = panel_preset(hostSet.panel); // the PANEL preset: the screen the core lays out for
  if (fixed)
    snprintf(v, sizeof v, "%s", fixed);
  else
    snprintf(v, sizeof v, "%dx%d", ow, oh);
  snprintf(windowValue, sizeof windowValue, "%dx%d", ow, oh);
  if (strcmp(v, panelValue)) {
    snprintf(panelValue, sizeof panelValue, "%s", v);
    optionsChanged = true;
  }
}
static bool state_file(const char *ss, bool save) {
  if (save) {
    size_t n = core.serialize_size();
    void *d = n ? malloc(n) : NULL;
    bool ok = d && core.serialize(d, n) && write_file(ss, d, n, false);
    free(d);
    return ok;
  }
  size_t n;
  unsigned char *d = read_file(ss, &n, 64u << 20);
  bool ok = d && core.unserialize(d, n);
  free(d);
  if (ok)
    rewind_clear(); // its differences lead back from another history
  return ok;
}
/* The .sav: the core's save memory, then its cartridge clock (RETRO_MEMORY_RTC: a Game Boy MBC3's, Crystal's) right
 * after it - mGBA's own layout, which the old host wrote too. NULL out: the size. */
static size_t save_image(unsigned char *out) {
  void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM), *rtc = core.get_memory_data(RETRO_MEMORY_RTC);
  size_t n = mem ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0, rn = rtc ? core.get_memory_size(RETRO_MEMORY_RTC) : 0;
  if (out && n)
    memcpy(out, mem, n);
  if (out && n && rn)
    memcpy(out + n, rtc, rn);
  return n ? n + rn : 0;
}
// The file's bytes into the core: the save memory, and what follows it into the clock (a GBA .sav's 16-byte clock
// footer from mGBA finds no clock memory on armGBA, so a dead battery recorded there never comes back).
static void save_load(const unsigned char *d, size_t fn) {
  void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM), *rtc = core.get_memory_data(RETRO_MEMORY_RTC);
  size_t n = mem ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0, rn = rtc ? core.get_memory_size(RETRO_MEMORY_RTC) : 0;
  if (!n)
    return;
  memcpy(mem, d, fn < n ? fn : n);
  if (rn && fn > n)
    memcpy(rtc, d + n, fn - n < rn ? fn - n : rn);
}
struct save_baseline {
  unsigned char *data;
  size_t size;
};
static void sram_write(const char *sav, struct save_baseline *copy, bool sync) {
  static unsigned char *now;
  static size_t nowSize;
  size_t n = save_image(NULL);
  if (!n)
    return;
  if (n > nowSize) {
    free(now);
    now = malloc(n);
    nowSize = now ? n : 0;
    if (!now)
      return;
  }
  save_image(now);
  if (n == copy->size && !memcmp(now, copy->data, n))
    return;
  if (!write_file(sav, now, n, sync)) {
    notice("Save write failed");
    return;
  }
  unsigned char *previous = copy->data;
  size_t previousSize = copy->size;
  copy->data = now;
  copy->size = n;
  now = previous;
  nowSize = previousSize;
}
// The settings the old host kept in DATA/settings.ini become the core's (SAVES/pokedaisy/settings.ini), once.
static void settings_migrate(void) {
  char from[PATH_MAX + 16], dir[PATH_MAX + 16], to[PATH_MAX + 32];
  snprintf(from, sizeof from, "%s/settings.ini", root);
  snprintf(dir, sizeof dir, "%s/pokedaisy", savesDir);
  snprintf(to, sizeof to, "%s/settings.ini", dir);
  size_t n;
  unsigned char *d = exists(to) ? NULL : read_file(from, &n, 1u << 20);
  if (!d)
    return;
  pd_mkdir(dir);
  if (write_file(to, d, n, false))
    fprintf(stderr, "Settings moved from %s to %s\n", from, to);
  free(d);
}
static const char *core_for(const char *path) {
  struct pd_zip_entry zipped;
  if (pd_zip_is_zip(path)) // a zipped game: the core for the game inside it
    return pd_zip_game(path, &zipped) && !pd_zip_is_zip(zipped.name) ? core_for(zipped.name) : NULL;
  const char *ext = strrchr(path, '.');
  if (ext && !strcasecmp(ext, ".gba"))
#ifdef __APPLE__
    // macOS: armGBA's recompiler needs RWX memory macOS refuses (no MAP_JIT path); mGBA's core runs GBA with the same companion.
    return "pokedaisy_gbc_libretro" PD_CORE_EXT;
#else
    return "pokedaisy_gba_libretro" PD_CORE_EXT;
#endif
  if (ext && (!strcasecmp(ext, ".gb") || !strcasecmp(ext, ".gbc")))
    return "pokedaisy_gbc_libretro" PD_CORE_EXT;
  if (ext && !strcasecmp(ext, ".nds"))
    return "pokedaisy_ds_libretro" PD_CORE_EXT;
  return NULL;
}
/* The game list shows the main Pokemon games only (the firmware's folders hold every other game too), told by the cart's
 * header: a GBA game code of FireRed / LeafGreen / Emerald / Ruby / Sapphire (the hacks keep theirs) or a hack's own,
 * a Game Boy title of Red / Blue / Green / Yellow / Gold / Silver / Crystal and their hacks (not Pinball, the card
 * games or Puzzle Challenge), a DS game code of Diamond / Pearl / Platinum / HeartGold / SoulSilver / Black / White /
 * their sequels. */
static bool pokemon_rom(const char *path) {
  unsigned char h[0x150];
  FILE *f = fopen(path, "rb");
  size_t n = f ? fread(h, 1, sizeof h, f) : 0;
  if (f)
    fclose(f);
  const char *ext = strrchr(path, '.');
  if (!ext)
    return false;
  if (!strcasecmp(ext, ".gba")) {
    // Retail's first three letters (the fourth is the language); a hack's own code whole (Monomyth, Ultra Violet).
    static const char *const codes[] = {"BPR", "BPG", "BPE", "AXV", "AXP", "MONE", "PUVV"};
    for (size_t i = 0; n >= 0xB0 && i < sizeof codes / sizeof *codes; i++)
      if (!memcmp(h + 0xAC, codes[i], strlen(codes[i])))
        return true;
    return false;
  }
  if (!strcasecmp(ext, ".nds")) {
    static const char *const codes[] = {"ADA", "APA", "CPU", "IPK", "IPG", "IRB", "IRA", "IRE", "IRD"};
    for (size_t i = 0; n >= 0x10 && i < sizeof codes / sizeof *codes; i++)
      if (!memcmp(h + 0x0C, codes[i], 3))
        return true;
    return false;
  }
  if (n < 0x144)
    return false;
  char title[17] = {0};
  memcpy(title, h + 0x134, 16);
  static const char *const spinOffs[] = {"POKECARD", "POKEPINBALL", "POKEMON PC", "POKEMONPC", "POKEMON-CG", "POKEMON PINB"};
  for (size_t i = 0; i < sizeof spinOffs / sizeof *spinOffs; i++)
    if (!strncmp(title, spinOffs[i], strlen(spinOffs[i])))
      return false;
  return !strncmp(title, "POKE", 4) || !strncmp(title, "PM_", 3) || !strncmp(title, "PK", 2);
}
#ifdef POCKET_DSPERATE
extern int pd_ds_run(const char *, const char *, int, const char *, SDL_Renderer *, bool (*)(void));
static bool ds_running(void) { return running != 0; }
#endif
// The host menu's rows, top to bottom (fast-forward is the FAST FORWARD hotkey's, held).
enum { M_RESUME, M_SAVE, M_LOAD, M_HOTKEYS, M_KEYBOARD, M_REWIND, M_FFSPEED, M_LCD, M_SWAP, M_QUIT,
       M_COUNT };
/* A zipped game's copy to play: DATA/unzipped/<the game's file name>, unpacked when it isn't there yet or the zip is newer. */
static bool unzip_game(const char *zip, char *out, size_t size) {
  struct pd_zip_entry e;
  if (!pd_zip_game(zip, &e))
    return false;
  char dir[PATH_MAX + 16];
  snprintf(dir, sizeof dir, "%s/unzipped", root);
  pd_mkdir(dir);
  snprintf(out, size, "%s/%s", dir, e.name);
  struct stat z, u;
  if (!stat(zip, &z) && !stat(out, &u) && (uint64_t)u.st_size == e.usize && u.st_mtime >= z.st_mtime)
    return true;
  return pd_zip_extract(zip, &e, out);
}
/* Save-state slots: SLOT 1 is the state file the host always had (states/<game>.ss), 2-4 beside it (.s2.ss ...), and
 * slot 0 the one written when the game is left (.auto.ss: CONTINUE ON OPEN, LOAD STATE > WHERE YOU QUIT). Each has a
 * small picture of the game beside it (<state>.bmp). The hotkeys and the menu bar use the slot last saved to. */
#define SLOTS 4
static int slotNow = 1;
static void slot_path(const char *ss, int slot, char *out, size_t size) {
  int stem = (int)strlen(ss) - 3; // without ".ss"
  if (slot == 1)
    snprintf(out, size, "%s", ss);
  else if (slot)
    snprintf(out, size, "%.*s.s%d.ss", stem, ss, slot);
  else
    snprintf(out, size, "%.*s.auto.ss", stem, ss);
}
// The frame as it is now, small, into <state>.bmp: drawn to the back buffer (the next present clears it) and read back.
static void thumb_write(const char *state) {
  if (!frameTexture || frameW <= 0 || frameH <= 0)
    return;
  int tw = 192, th = tw * frameH / frameW;
  if (th > 192)
    th = 192, tw = th * frameW / frameH;
  SDL_Surface *img = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!img)
    return;
  SDL_Rect src = {0, 0, frameW, frameH}, dst = {0, 0, tw, th};
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, frameTexture, &src, &dst);
  char bmp[PATH_MAX * 2 + 8];
  snprintf(bmp, sizeof bmp, "%s.bmp", state);
  if (!SDL_RenderReadPixels(renderer, &dst, SDL_PIXELFORMAT_ARGB8888, img->pixels, img->pitch)) {
    // Opaque: the DS core's frames are XRGB with the X byte 0, which read back as a fully clear picture.
    for (int y = 0; y < th; y++)
      for (int x = 0; x < tw; x++)
        ((uint32_t *)((uint8_t *)img->pixels + y * img->pitch))[x] |= 0xFF000000u;
    SDL_SaveBMP(img, bmp);
  }
  SDL_FreeSurface(img);
}
static void slot_thumb_load(const char *state) {
  if (slotThumb)
    SDL_DestroyTexture(slotThumb);
  slotThumb = NULL;
  char bmp[PATH_MAX * 2 + 8];
  snprintf(bmp, sizeof bmp, "%s.bmp", state);
  SDL_Surface *img = exists(state) ? SDL_LoadBMP(bmp) : NULL;
  if (!img)
    return;
  slotThumb = SDL_CreateTextureFromSurface(renderer, img);
  if (slotThumb) // a picture saved before it was made opaque still shows
    SDL_SetTextureBlendMode(slotThumb, SDL_BLENDMODE_NONE);
  slotThumbW = img->w;
  slotThumbH = img->h;
  SDL_FreeSurface(img);
}
static bool slot_save(const char *ss, int slot) {
  char path[PATH_MAX * 2 + 16];
  slot_path(ss, slot, path, sizeof path);
  if (!state_file(path, true))
    return false;
  thumb_write(path);
  return true;
}
// The slot pages' rows: SLOT 1..4 with when each was saved (or EMPTY), and LOAD STATE's WHERE YOU QUIT.
static void slot_rows(const char *ss, char rows[][48], const char **ptrs, int count) {
  for (int i = 0; i < count; i++) {
    char path[PATH_MAX * 2 + 16], when[32] = "EMPTY";
    const int slot = i < SLOTS ? i + 1 : 0;
    slot_path(ss, slot, path, sizeof path);
    struct stat st;
    if (!stat(path, &st)) {
      strftime(when, sizeof when, "%b %d %I:%M %p", localtime(&st.st_mtime));
      for (char *q = when; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    }
    if (slot)
      snprintf(rows[i], 48, "SLOT %d  %s", slot, when);
    else
      snprintf(rows[i], 48, "WHERE YOU QUIT  %s", when);
    ptrs[i] = rows[i];
  }
}
// F12 / Cmd+S in a game: the screen as it is, Pictures/PokeDaisy/<game> <date>.png (DATA/screenshots off a Mac).
static void screenshot_save(const char *game) {
  char dir[PATH_MAX], path[PATH_MAX * 2], stamp[48];
  const char *home = getenv("HOME");
#ifdef __APPLE__
  if (home) {
    snprintf(dir, sizeof dir, "%s/Pictures", home);
    pd_mkdir(dir);
    snprintf(dir, sizeof dir, "%s/Pictures/PokeDaisy", home);
  } else
#endif
    snprintf(dir, sizeof dir, "%s/screenshots", root);
  (void)home;
  pd_mkdir(dir);
  const char *file = strrchr(game, '/') ? strrchr(game, '/') + 1 : game, *dot = strrchr(file, '.');
  time_t now = time(NULL);
  strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", localtime(&now));
  snprintf(path, sizeof path, "%s/%.*s %s.png", dir, dot && dot != file ? (int)(dot - file) : (int)strlen(file), file, stamp);
#ifdef __APPLE__
  notice(capture(path) ? "Screenshot saved in Pictures > PokeDaisy" : "Screenshot failed");
#else
  notice(capture(path) ? "Screenshot saved in DATA/screenshots" : "Screenshot failed");
#endif
}
static void run_game_play(const char *path, int maxFrames, const char *shot, bool fromList) {
  listShown = false;
  // The save and the states are named after the file in the list (a zip's own name); the core plays its game unpacked.
  const char *named = path;
  char unpacked[PATH_MAX * 2];
  if (pd_zip_is_zip(path)) {
    if (!unzip_game(path, unpacked, sizeof unpacked)) {
      appError = 1;
      notice("No game in that zip could be opened");
      return;
    }
    path = unpacked;
  }
#ifdef POCKET_DSPERATE
  const char *ext = strrchr(path, '.');
  if (ext && !strcasecmp(ext, ".nds")) {
    if (audio) {
      SDL_CloseAudioDevice(audio);
      audio = 0;
    }
    appError = pd_ds_run(path, root, maxFrames, shot, renderer, ds_running);
    // The DS backend owns its device while running; the GB / GBA games get theirs back.
    audio_open();
    audioLost = !audio;
    held = pressed = 0;
    return;
  }
#endif
  const char *coreName = core_for(path);
  char corePath[PATH_MAX * 2];
  const char *dir = getenv("POCKET_CORE_DIR") ? getenv("POCKET_CORE_DIR") : exeDir;
  snprintf(corePath, sizeof corePath, "%s/%s", dir, coreName ? coreName : "");
  if (!coreName || !core_open(corePath)) {
    appError = 1;
    notice(coreName ? "Core missing: put it beside " PD_HOST_NAME : "Unsupported ROM");
    return;
  }
  struct retro_system_info system = {0};
  core.get_system_info(&system);
  size_t romSize = 0;
  unsigned char *rom = system.need_fullpath ? NULL : read_file(path, &romSize, 64u << 20);
  if (!system.need_fullpath && (!rom || romSize < 0x150)) {
    free(rom);
    dlclose(core.dl);
    appError = 1;
    notice("Cannot read ROM");
    return;
  }
  // The ROM's file name: its whole name with the extension, without the folder. A Windows path may use either slash
  // (a command line launch or a shortcut passes backslashes), and the save's name follows the ROM's.
  const char *name = strrchr(named, '/');
#ifdef _WIN32
  const char *back = strrchr(named, '\\');
  if (back && (!name || back > name))
    name = back;
#endif
  name = name ? name + 1 : named;
  char sav[PATH_MAX * 2], ss[PATH_MAX * 2], backups[PATH_MAX * 2];
  if (saveFile) {
    snprintf(sav, sizeof sav, "%s", saveFile);
    snprintf(backups, sizeof backups, "%s/backups", root);
  } else {
    pd_save_path(savesDir, named, sav, sizeof sav);
    snprintf(backups, sizeof backups, "%s/pokedaisy-backups", savesDir);
  }
  // DS uses L2 for the Companion and R2 for screen switching. Keep the host's DS bindings separate.
  const bool dsGame = !strcmp(coreName, "pokedaisy_ds_libretro" PD_CORE_EXT);
  int *hotkeys = dsGame ? hostSet.dsHotkey : hostSet.hotkey;
  // armGBA's states aren't mGBA's: each core its own file.
  snprintf(ss, sizeof ss, "%s/states/%s%s.ss", root, name, strstr(coreName, "_gba_") ? ".armgba" : "");
  if (strstr(coreName, "_gba_")) {
    // A GBA state from before armGBA is mGBA's: it can't load here, so say so instead of "No state saved yet".
    char old[PATH_MAX * 2 + 16];
    snprintf(old, sizeof old, "%s/states/%s.ss", root, name);
    if (exists(old) && !exists(ss))
      notice("Your older save state doesn't load in this version");
  }
  if (!save_backup(sav, backups)) {
    free(rom);
    dlclose(core.dl);
    appError = 1;
    notice("Save backup failed; game not opened");
    return;
  }
  settings_migrate();
  optionN = 0;
  pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
  panel_follow();
  optionsChanged = false;
  core.set_environment(environment);
  core.init();
  core.set_video_refresh(video_refresh);
  core.set_audio_sample(audio_sample);
  core.set_audio_sample_batch(audio_batch);
  core.set_input_poll(input_poll);
  core.set_input_state(input_state);
  struct retro_game_info info = {path, rom, romSize, NULL};
  if (!core.load_game(&info)) {
    core.deinit();
    dlclose(core.dl);
    free(rom);
    appError = 1;
    notice("ROM load failed");
    return;
  }
  core.get_system_av_info(&av);
  srcRate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32768;
  double hz = av.timing.fps > 0 ? av.timing.fps : 59.7275;
  frameW = frameH = 0;
  frame_texture();
  // The battery save: the file's bytes into the core's, and written back whenever they change.
  struct save_baseline sramCopy = {0};
  {
    size_t n = save_image(NULL), fn;
    unsigned char *d = n ? read_file(sav, &fn, 64u << 20) : NULL;
    if (d)
      save_load(d, fn);
    free(d);
    sramCopy.data = n ? malloc(n) : NULL;
    if (sramCopy.data)
      sramCopy.size = save_image(sramCopy.data);
  }
  // CONTINUE ON OPEN: the state written when the game was last left, unless the save is newer (played elsewhere since:
  // a cloud folder's save from another computer, or the game ended without its state).
  char autoSS[PATH_MAX * 2 + 16];
  slot_path(ss, 0, autoSS, sizeof autoSS);
  {
    struct stat a, v;
    if (hostSet.resume && !stat(autoSS, &a)) {
      if (!stat(sav, &v) && v.st_mtime > a.st_mtime)
        notice("Your save is newer than where you quit: started from your save");
      else if (state_file(autoSS, false))
        notice("Picked up where you left off");
    }
  }
  int rewindMB = getenv("POCKET_REWIND_MB") ? atoi(getenv("POCKET_REWIND_MB")) : 32;
  rewindBudget = (size_t)(rewindMB > 0 ? rewindMB : 32) << 20; // a bad value (negative, 0) is the default
  rewindTick = 0;
  bool menuOpen = false, leave = false;
  struct pd_menu menu, hkMenu;
  static char rows[M_COUNT][40], hkRows[HK_ACTIONS + 1][40];
  static const char *rowPtrs[M_COUNT], *hkPtrs[HK_ACTIONS + 1];
  static const char *const hkNames[HK_ACTIONS] = {"FAST FORWARD", "REWIND", "SAVE STATE", "LOAD STATE"};
  bool hkPage = false, kbPage = false;
  unsigned latched = 0, menuConsumed = 0;
  bool modifierPending = false, modifierUsed = false;
  int pulseLeft = 0; // frames the modifier's own tap still reaches the game
  int capturing = -1; // the HOTKEYS row waiting for a button
  // LOAD STATE and QUIT ask first, CANCEL on top (as the hosts before this one): a stray A never loses the game's progress.
  struct pd_menu confirmMenu;
  static const char *confirmRows[2];
  int confirming = 0; // the menu row asked about (M_LOAD / M_QUIT), 0 = none
  int slotPage = 0;   // SAVE STATE's slots (1) or LOAD STATE's (2), 0 = none
  struct pd_menu slotMenu;
  static char slotRowText[SLOTS + 1][48];
  static const char *slotPtrs[SLOTS + 1];
  char loadPath[PATH_MAX * 2 + 16]; // the state LOAD STATE's question is about
  snprintf(loadPath, sizeof loadPath, "%s", ss);
  int thumbFor = -1; // the slot whose picture is loaded
  bool pausedAway = false;
  int frames = 0;
  uint64_t due = SDL_GetPerformanceCounter(), lastStart = 0, lastFrame = due;
  ffQuick = false; // a new game starts at normal speed
  pacing_start(hz);
  while (running && !leave) {
    uint64_t frameStart = SDL_GetPerformanceCounter();
    // Back from a pause (a stopped process, sleep): no catching up.
    if (lastStart && (frameStart - lastStart) * 4 > SDL_GetPerformanceFrequency())
      due = frameStart;
    lastStart = frameStart;
    events();
    audio_check();
    repeat_directions();
    if (outputChanged) {
      outputChanged = false;
      panel_follow();
    }
    // PAUSE IN BACKGROUND: nothing runs while another window has the focus; the picture stays.
    if (windowAway && hostSet.pauseAway && !maxFrames) {
      if (!pausedAway)
        notice("Paused: click the window to play");
      pausedAway = true;
      muted = true;
      if (audio)
        SDL_ClearQueuedAudio(audio);
      present_game(NULL);
      SDL_Delay(30);
      due = SDL_GetPerformanceCounter();
      continue;
    }
    pausedAway = false;
    const unsigned menuChord = (1u << START) | (1u << SELECT);
    const bool menuChordDown = (held & menuChord) == menuChord;
    bool menuKey = down(MENU) || (comboMenu && menuChordDown && (pressed & menuChord));
    snprintf(rows[M_RESUME], 40, "RESUME");
    snprintf(rows[M_SAVE], 40, "SAVE STATE");
    snprintf(rows[M_LOAD], 40, "LOAD STATE");
    if (hostSet.ffSpeed)
      snprintf(rows[M_FFSPEED], 40, "FF SPEED  %dx", hostSet.ffSpeed);
    else
      snprintf(rows[M_FFSPEED], 40, "FF SPEED  MAX");
    snprintf(rows[M_REWIND], 40, "REWIND  %s", hostSet.rewind ? "ON" : "OFF");
    snprintf(rows[M_LCD], 40, "LCD FILTER  %s", hostSet.lcd ? "ON" : "OFF");
    snprintf(rows[M_SWAP], 40, "A/B BUTTONS  %s", hostSet.swapAB ? "SWAPPED" : "NORMAL");
    snprintf(rows[M_HOTKEYS], 40, "HOTKEYS");
    snprintf(rows[M_KEYBOARD], 40, "KEYBOARD");
    snprintf(rows[M_QUIT], 40, fromList ? "QUIT TO GAME LIST" : "QUIT");
    for (int i = 0; i < M_COUNT; i++)
      rowPtrs[i] = rows[i];
    for (int i = 0; i < HK_ACTIONS; i++) {
      snprintf(hkRows[i], 40, "%s  %s", hkNames[i], capturing == i ? "..." : hotkey_label(hotkeys[i]));
      hkPtrs[i] = hkRows[i];
    }
    snprintf(hkRows[HK_ACTIONS], 40, "MODIFIER  %s",
             capturing == HK_ACTIONS ? "..." : hotkey_label(hostSet.modifier));
    hkPtrs[HK_ACTIONS] = hkRows[HK_ACTIONS];
    for (int i = 0; i < KB_COUNT; i++) {
      const char *name = kbKeys[i] ? SDL_GetKeyName((SDL_Keycode)kbKeys[i]) : "OFF";
      snprintf(kbRows[i], 48, "%s  %s", kbLabels[i], kbCapture == i ? "..." : name);
      kbPtrs[i] = kbRows[i];
    }
    kb_menu_build();
    menuConsumed &= held;
    if (inputReset || menuKey || menuChordDown) {
      modifierPending = modifierUsed = false;
      pulseLeft = 0;
      inputReset = false;
      if (menuKey || menuChordDown)
        menuConsumed |= held;
    }
    if (menuKey) {
      menuOpen = !menuOpen;
      hkPage = false, kbPage = false;
      confirming = 0;
      slotPage = 0;
      capturing = -1;
      kbCapture = -1;
      if (menuOpen) {
        pd_menu_init(&menu, "POKEDAISY", rowPtrs, M_COUNT);
        menu.overlay = true;
      }
    }
    unsigned hkFired = 0, hkOn = 0;
    hotkeyMask = 0;
    unsigned modifier = hostSet.modifier < 0 ? 0 : 1u << hostSet.modifier;
    latched &= held;
    unsigned modifierPulse = 0;
    if (!menuOpen && !menuKey && !menuChordDown) {
      if (pressed & modifier & ~menuConsumed)
        modifierPending = true;
      if (!(menuConsumed & modifier))
        hkOn = hotkeys_held(hotkeys, modifier, &latched, &hkFired);
      // The DS core handles its stylus shortcuts with Select held.
      const unsigned stylus = (1u << UP) | (1u << DOWN) | (1u << LEFT) | (1u << RIGHT) | (1u << A) | (1u << Y);
      bool dsStylus = dsGame && hostSet.modifier == SELECT && (held & modifier)
                      && (held & stylus & ~latched & ~menuConsumed) && !(latched & hotkeyMask);
      modifierUsed |= !!(latched & hotkeyMask) || dsStylus;
      if (!(held & modifier)) {
        if (modifierPending && !modifierUsed)
          pulseLeft = MODIFIER_TAP_FRAMES;
        modifierPending = modifierUsed = false;
      } else
        pulseLeft = 0; // held again: it's the modifier now, not a tap
      if (pulseLeft > 0)
        modifierPulse = modifier;
      if (!dsStylus)
        hotkeyMask |= modifier;
    } else
      pulseLeft = 0;
    hotkeyMask |= latched | menuConsumed;
    if (hkFired & ((1u << HK_SAVE) | (1u << HK_LOAD))) { // the slot last saved to
      char slotFile[PATH_MAX * 2 + 16], said[48];
      slot_path(ss, slotNow, slotFile, sizeof slotFile);
      if (hkFired & (1u << HK_SAVE))
        snprintf(said, sizeof said, slot_save(ss, slotNow) ? "State saved (slot %d)" : "State save failed", slotNow);
      else
        snprintf(said, sizeof said, !exists(slotFile) ? "No state in slot %d yet"
                                    : state_file(slotFile, false) ? "State loaded (slot %d)" : "State load failed", slotNow);
      notice(said);
    }
    if ((hkFired & (1u << HK_REWIND)) && !hostSet.rewind)
      notice("REWIND is off: turn it on in the menu");
    // The mouse in the menu: a click on a row picks it as A would, a right click is B. (Taken each frame, so a click
    // in the game, the core's touch, isn't kept for the menu.)
    const bool click = libraryClick, rightClick = libraryRightClick;
    libraryClick = libraryRightClick = false;
    if (menuOpen && capturing < 0 && kbCapture < 0 && (click || rightClick)) {
      struct pd_menu *shown = confirming ? &confirmMenu : slotPage ? &slotMenu : hkPage ? &hkMenu : kbPage ? &kbMenu : &menu;
      int ow = W, oh = H;
      SDL_GetRendererOutputSize(renderer, &ow, &oh);
      int sc = overlay.w > 0 && overlay.h > 0 ? (ow / overlay.w < oh / overlay.h ? ow / overlay.w : oh / overlay.h) : 1;
      sc = sc < 1 ? 1 : sc;
      const int x = (mouseWindowX - (ow - overlay.w * sc) / 2) / sc, y = (mouseWindowY - (oh - overlay.h * sc) / 2) / sc;
      if (rightClick)
        pressed |= 1u << B;
      else {
        pd_menu_touch(shown, x, y, true);
        const int row = pd_menu_touch(shown, x, y, false);
        if (row >= 0) {
          shown->selected = row;
          pressed |= 1u << A;
        }
      }
    }
    const bool menuFrame = menuOpen; // the frame that closes the menu is still the menu's
    if (menuOpen && capturing >= 0) {
      // The next assignable button is the hotkey (taken from any other action that had it); B cancels, SELECT = OFF.
      if (down(B))
        capturing = -1;
      else if (down(SELECT)) {
        if (capturing == HK_ACTIONS) {
          hostSet.modifier = hostSet.modifier == SELECT ? -1 : SELECT;
          if (hostSet.modifier >= 0)
            modifier_takes(hostSet.modifier);
          ds_hotkeys_follow_modifier();
        } else
          hotkeys[capturing] = -1;
        host_save();
        capturing = -1;
      } else
        for (int k = 0; k < NKEYS; k++)
          if (down(k) && (hotkey_assignable(k) || (capturing == HK_ACTIONS && k == START))) {
            if (capturing == HK_ACTIONS) {
              hostSet.modifier = k;
              modifier_takes(k);
              ds_hotkeys_follow_modifier();
            } else if (k == hostSet.modifier) {
              notice("That button is the MODIFIER");
              capturing = -1;
              break;
            } else {
              for (int j = 0; j < HK_ACTIONS; j++)
                if (hotkeys[j] == k)
                  hotkeys[j] = -1;
              hotkeys[capturing] = k;
            }
            host_save();
            capturing = -1;
            break;
          }
      hkMenu.title = capturing >= 0 ? "PRESS A BUTTON" : "HOTKEYS";
    } else if (menuOpen && confirming) {
      if (down(UP))
        pd_menu_move(&confirmMenu, -1);
      if (down(DOWN))
        pd_menu_move(&confirmMenu, 1);
      if (down(B))
        confirming = 0;
      if (down(A)) {
        if (confirmMenu.selected == 1 && confirming == M_LOAD) {
          notice(state_file(loadPath, false) ? "State loaded" : "State load failed");
          menuOpen = false;
          slotPage = 0;
        } else if (confirmMenu.selected == 1 && confirming == M_QUIT) {
          leave = true;
        }
        confirming = 0;
      }
    } else if (menuOpen && slotPage) {
      if (down(UP))
        pd_menu_move(&slotMenu, -1);
      if (down(DOWN))
        pd_menu_move(&slotMenu, 1);
      if (down(B))
        slotPage = 0;
      else if (down(A)) {
        const int slot = slotMenu.selected < SLOTS ? slotMenu.selected + 1 : 0;
        char slotFile[PATH_MAX * 2 + 16], said[48];
        slot_path(ss, slot, slotFile, sizeof slotFile);
        if (slotPage == 1) {
          snprintf(said, sizeof said, slot_save(ss, slot) ? "State saved (slot %d)" : "State save failed", slot);
          slotNow = slot;
          notice(said);
          menuOpen = false;
          slotPage = 0;
        } else if (!exists(slotFile)) {
          notice("That slot is empty");
        } else {
          snprintf(loadPath, sizeof loadPath, "%s", slotFile);
          if (slot)
            slotNow = slot;
          confirming = M_LOAD;
          confirmRows[0] = "CANCEL";
          confirmRows[1] = "LOAD STATE";
          pd_menu_init(&confirmMenu, "LOAD THE STATE?", confirmRows, 2);
          confirmMenu.overlay = true;
        }
      }
    } else if (menuOpen && hkPage) {
      if (down(UP))
        pd_menu_move(&hkMenu, -1);
      if (down(DOWN))
        pd_menu_move(&hkMenu, 1);
      if (down(B))
        hkPage = false;
      if (down(A)) {
        capturing = hkMenu.selected;
        hkMenu.title = "PRESS A BUTTON";
        notice(capturing == HK_ACTIONS ? "SELECT: select / off   B: cancel" : "B: cancel   SELECT: off");
      }
    } else if (menuOpen && kbCapture >= 0) {
      // The key that went down is the row's (taken from any other row that had it); B cancels, SELECT = off.
      // On a keyboard, Escape cancels and Backspace turns the row off (else they were bound, Escape taking MENU's key).
      if (lastKey == SDLK_ESCAPE)
        lastKey = 0, pressed |= 1u << B;
      else if (lastKey == SDLK_BACKSPACE || lastKey == SDLK_DELETE)
        lastKey = 0, pressed |= 1u << SELECT;
      if (lastKey) {
        for (int i = 0; i < KB_COUNT; i++)
          if (kbKeys[i] == lastKey)
            kbKeys[i] = SDLK_UNKNOWN;
        kbKeys[kbCapture] = lastKey;
        host_save();
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      } else if (down(B)) {
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      } else if (down(SELECT)) {
        kbKeys[kbCapture] = SDLK_UNKNOWN;
        host_save();
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      }
    } else if (menuOpen && kbPage) {
      if (down(UP))
        pd_menu_move(&kbMenu, -1);
      if (down(DOWN))
        pd_menu_move(&kbMenu, 1);
      if (down(B))
        kbPage = false;
      if (down(A)) {
        kbCapture = kbMenu.selected;
        held = pressed = 0; // the keys it took to get here don't assign themselves
        kbMenu.title = "PRESS A KEY";
        notice("Press a key. Esc: cancel   Backspace: off");
      }
    } else if (menuOpen) {
      if (down(UP))
        pd_menu_move(&menu, -1);
      if (down(DOWN))
        pd_menu_move(&menu, 1);
      if (down(B))
        menuOpen = false;
      if (down(A)) {
        switch (menu.selected) {
        case M_RESUME:
          menuOpen = false;
          break;
        case M_SAVE:
        case M_LOAD:
          slotPage = menu.selected == M_SAVE ? 1 : 2;
          slot_rows(ss, slotRowText, slotPtrs, slotPage == 1 ? SLOTS : SLOTS + 1);
          pd_menu_init(&slotMenu, slotPage == 1 ? "SAVE STATE: PICK A SLOT" : "LOAD STATE: PICK A SLOT", slotPtrs,
                       slotPage == 1 ? SLOTS : SLOTS + 1);
          slotMenu.overlay = true;
          slotMenu.selected = slotNow - 1;
          thumbFor = -1;
          break;
        case M_FFSPEED:
          hostSet.ffSpeed = hostSet.ffSpeed == 0 ? 2 : hostSet.ffSpeed >= 8 ? 0 : hostSet.ffSpeed + 1;
          host_save();
          break;
        case M_REWIND:
          hostSet.rewind = !hostSet.rewind;
          if (!hostSet.rewind)
            rewind_clear();
          host_save();
          break;
        case M_LCD:
          hostSet.lcd = !hostSet.lcd;
          host_save();
          break;
        case M_SWAP:
          hostSet.swapAB = swapFace = !hostSet.swapAB;
          held = 0; // the button that chose this is now the other one: let it go
          host_save();
          break;
        case M_HOTKEYS:
          hkPage = true, kbPage = false;
          pd_menu_init(&hkMenu, "HOTKEYS", hkPtrs, HK_ACTIONS + 1);
          hkMenu.overlay = true;
          break;
        case M_KEYBOARD:
          kbPage = true, hkPage = false;
          kb_menu_open();
          break;
        case M_QUIT:
          confirming = M_QUIT;
          confirmRows[0] = "CANCEL";
          confirmRows[1] = fromList ? "QUIT TO GAME LIST" : "QUIT";
          pd_menu_init(&confirmMenu, "LEAVE THE GAME?", confirmRows, 2);
          confirmMenu.overlay = true;
          break;
        }
      }
    }
    if (menuFrame) {
      muted = true;
      gameKeys = 0;
      slotThumbShow = menuOpen && slotPage && !confirming;
      if (slotThumbShow && thumbFor != slotMenu.selected) {
        char slotFile[PATH_MAX * 2 + 16];
        slot_path(ss, slotMenu.selected < SLOTS ? slotMenu.selected + 1 : 0, slotFile, sizeof slotFile);
        slot_thumb_load(slotFile);
        thumbFor = slotMenu.selected;
      }
      present_game(menuOpen ? (confirming ? &confirmMenu : slotPage ? &slotMenu : hkPage ? &hkMenu : kbPage ? &kbMenu : &menu)
                            : NULL);
      if (maxFrames && ++frames >= maxFrames) { // a scripted run can end, and be captured, with the menu up
        if (shot && !capture(shot))
          appError = 1;
        break;
      }
      if (!vsyncPaced && !maxFrames)
        SDL_Delay(16);
      due = SDL_GetPerformanceCounter();
      continue;
    }
    muted = false;
    bool rewinding = (hkOn & (1u << HK_REWIND)) && hostSet.rewind;
    bool ff = !rewinding && ((hkOn & (1u << HK_FF)) || ffQuick);
    int runs = 1;
    if (ff) {
      double cost = emuMs > 0 ? emuMs : 4;
      runs = hostSet.ffSpeed ? hostSet.ffSpeed : (int)(800.0 / hz / cost);
      runs = runs < 2 ? 2 : runs > 30 ? 30 : runs;
    }
    ffNow = ff;
    ffSpeedNow = runs;
    rewindingNow = rewinding;
    audioKeepEvery = (unsigned)runs;
    // The mouse as the app's touch screen: a press lasts a frame at least, so a quick click still taps.
    pointerDown = pointerHeld || pointerPulse > 0;
    if (pointerPulse > 0)
      pointerPulse--;
    gameKeys = joypad((held & ~hotkeyMask & ~((1u << MENU) | (1u << FFKEY) | (1u << RWKEY))) | modifierPulse);
    uint64_t t = SDL_GetPerformanceCounter();
    if (rewinding) {
      muted = true;
      if (rewind_pop())
        core.run();
      else
        rewinding = false;
    } else {
      for (int i = 0; i < runs && running; i++) {
        core.run();
        wheelSteps = 0; // the core has read this frame's notches
        if (modifierPulse && --pulseLeft <= 0) {
          gameKeys &= ~joypad(modifierPulse);
          modifierPulse = 0;
        }
        if (hostSet.rewind && !(++rewindTick & 1))
          rewind_push();
      }
    }
    emuMs = (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / SDL_GetPerformanceFrequency() / runs;
    if (++frames % 120 == 0)
      sram_write(sav, &sramCopy, true);
    present_game(NULL);
    if (screenshotWanted) {
      screenshotWanted = false;
      screenshot_save(named);
    }
    if (maxFrames && frames >= maxFrames) {
      if (shot && !capture(shot))
        appError = 1;
      break;
    }
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    pacing_check((double)(now - lastFrame) * 1000.0 / f);
    lastFrame = now;
    audio_trim(hz, vsyncPaced ? screenHz : hz);
    if (!vsyncPaced && !maxFrames) {
      due += (uint64_t)(f / hz);
      if (now < due)
        SDL_Delay((Uint32)((due - now) * 1000 / f));
      else if (now - due > f / 10)
        due = now; // far behind (a stall): start again rather than run to catch up
    }
    perf_frame(frameStart, hz);
  }
  sram_write(sav, &sramCopy, true);
  if (hostSet.resume && frames > 60 && state_file(autoSS, true)) // where it was left, for next time (after the save)
    thumb_write(autoSS);
  if (slotThumb)
    SDL_DestroyTexture(slotThumb);
  slotThumb = NULL;
  slotThumbShow = false;
  free(sramCopy.data);
  rewind_clear();
  core.unload_game();
  core.deinit();
  dlclose(core.dl);
  memset(&core, 0, sizeof core);
  free(rom);
  if (audio)
    SDL_ClearQueuedAudio(audio);
  muted = false;
  held = pressed = 0;
}
// A game, with its own settings (GAME SETTINGS) in place while it plays.
static void run_game(const char *path, int maxFrames, const char *shot, bool fromList) {
  game_settings_begin(path);
  run_game_play(path, maxFrames, shot, fromList);
  game_settings_end();
}

// By the file name, whichever folder it is in.
static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
/* ---- the library (library.c): the host's own SETTINGS rows, and its canvases and covers on the screen ---- */
static bool libraryKeyboard; // the game list's KEYBOARD row was picked: its loop opens the page
static bool libraryHotkeys;  // its HOTKEYS row: the controller's hotkeys page
static bool librarySaves;    // its SAVES FOLDER row: the folders page
enum {
  HR_LCD,
  HR_FF,
  HR_REWIND,
  HR_DS_ENGINE,
  HR_SAVES,
  HR_RESUME,
  HR_PAUSE,
  HR_AB,
  HR_KEYBOARD,
  HR_HOTKEYS,
  HR_CONTROLLER,
  HR_COUNT
};
static const char *lib_label(int row) {
  static const char *const LABELS[HR_COUNT] = {"LCD FILTER",  "FF SPEED",         "REWIND",
                                               "DS ENGINE",   "SAVES FOLDER",     "CONTINUE ON OPEN",
                                               "PAUSE IN BACKGROUND", "A/B BUTTONS", "KEYBOARD",
                                               "HOTKEYS",     "CONTROLLER"};
  return LABELS[row];
}
static const char *lib_group(int row) {
  return row == HR_LCD ? "SCREEN" : row <= HR_DS_ENGINE ? "FF / SPEED" : row <= HR_PAUSE ? "SAVES / PLAY" : "CONTROLS";
}
static void saves_folder_name(char *out, int len); // SAVES FOLDER's value (below, with its page)
static void lib_value(int row, char *out, int len) {
  if (row == HR_LCD)
    snprintf(out, (size_t)len, "%s", hostSet.lcd ? "ON" : "OFF");
  else if (row == HR_FF && hostSet.ffSpeed)
    snprintf(out, (size_t)len, "%dx", hostSet.ffSpeed);
  else if (row == HR_FF)
    snprintf(out, (size_t)len, "MAX");
  else if (row == HR_REWIND)
    snprintf(out, (size_t)len, "%s", hostSet.rewind ? "ON" : "OFF");
  else if (row == HR_SAVES)
    saves_folder_name(out, len);
  else if (row == HR_RESUME)
    snprintf(out, (size_t)len, "%s", hostSet.resume ? "ON" : "OFF");
  else if (row == HR_PAUSE)
    snprintf(out, (size_t)len, "%s", hostSet.pauseAway ? "ON" : "OFF");
  else if (row == HR_DS_ENGINE)
    snprintf(out, (size_t)len, "%s", hostSet.dsSafe ? "SAFE" : "FAST");
  else if (row == HR_AB)
    snprintf(out, (size_t)len, "%s", hostSet.swapAB ? "SWAPPED" : "NORMAL");
  else if (row == HR_CONTROLLER) { // the controller in use, so a pad that isn't picked up shows here, not only in the log
    const char *name = controller ? SDL_GameControllerName(controller) : NULL;
    if (name)
      snprintf(out, (size_t)len, "%s", name);
    else if (unmappedPad[0])
      snprintf(out, (size_t)len, "%s: NOT SUPPORTED", unmappedPad);
    else
      snprintf(out, (size_t)len, "NONE");
    for (char *q = out; *q; q++)
      *q = (char)toupper((unsigned char)*q);
  } else
    snprintf(out, (size_t)len, "EDIT");
}
static void lib_step(int row, int dir) {
  if (row == HR_LCD)
    hostSet.lcd = !hostSet.lcd;
  else if (row == HR_FF) {
    // MAX, 2x ... 8x, round.
    int v = hostSet.ffSpeed ? hostSet.ffSpeed - 1 : 0; // 0 MAX, 1..7 = 2x..8x
    v = (v + dir + 8) % 8;
    hostSet.ffSpeed = v ? v + 1 : 0;
  } else if (row == HR_REWIND)
    hostSet.rewind = !hostSet.rewind;
  else if (row == HR_RESUME)
    hostSet.resume = !hostSet.resume;
  else if (row == HR_PAUSE)
    hostSet.pauseAway = !hostSet.pauseAway;
  else if (row == HR_DS_ENGINE) // read when a DS game opens
    hostSet.dsSafe = !hostSet.dsSafe;
  else if (row == HR_AB)
    hostSet.swapAB = swapFace = !hostSet.swapAB;
  else if (row == HR_CONTROLLER)
    return; // shows the controller; nothing to change
  else {
    // the loop opens the page over the list
    *(row == HR_KEYBOARD ? &libraryKeyboard : row == HR_HOTKEYS ? &libraryHotkeys : &librarySaves) = true;
    return;
  }
  host_save();
}
#ifndef POCKET_VERSION
#define POCKET_VERSION "dev"
#endif
static const struct lib_host libraryHost = {HR_COUNT, lib_label, lib_group, lib_value, lib_step, POCKET_VERSION};

/* The game list's HOTKEYS page: the controller buttons for FAST FORWARD, REWIND, SAVE STATE and LOAD STATE, for the
 * Game Boy / GBA games and the DS games apart (as a game's own host menu sets them), and the MODIFIER held with them. */
enum { HKL_DS = HK_ACTIONS, HKL_MODIFIER = 2 * HK_ACTIONS, HKL_ROWS };
static char hkLibRows[HKL_ROWS][48];
static const char *hkLibPtrs[HKL_ROWS];
static struct pd_menu hkLibMenu;
static int hkLibCapture = -1; // the row waiting for a button
static void hk_lib_build(void) {
  static const char *const names[HK_ACTIONS] = {"FAST FORWARD", "REWIND", "SAVE STATE", "LOAD STATE"};
  for (int i = 0; i < HKL_ROWS; i++) {
    const int *set = i < HKL_DS ? hostSet.hotkey : hostSet.dsHotkey;
    const char *value = hkLibCapture == i ? "..." : i == HKL_MODIFIER ? hotkey_label(hostSet.modifier) : hotkey_label(set[i % HK_ACTIONS]);
    snprintf(hkLibRows[i], sizeof hkLibRows[i], "%s%s  %s", i >= HKL_DS && i < HKL_MODIFIER ? "DS " : "",
             i == HKL_MODIFIER ? "MODIFIER" : names[i % HK_ACTIONS], value);
    hkLibPtrs[i] = hkLibRows[i];
  }
  hkLibMenu.title = hkLibCapture >= 0 ? "PRESS A BUTTON  (B: CANCEL)" : "HOTKEYS";
}
static void hk_lib_open(void) {
  hkLibCapture = -1;
  hk_lib_build();
  pd_menu_init(&hkLibMenu, "HOTKEYS", hkLibPtrs, HKL_ROWS);
  hkLibMenu.overlay = true;
}
/* The button pressed while a row waits: the same rules as the game's HOTKEYS page (B cancels, SELECT turns the row off,
 * a button taken from the action that had it, never the MODIFIER's own). */
static void hk_lib_capture(void) {
  const int row = hkLibCapture;
  int *set = row < HKL_DS ? hostSet.hotkey : hostSet.dsHotkey;
  if (down(B) || down(MENU)) {
    hkLibCapture = -1;
    return;
  }
  if (down(SELECT)) {
    if (row == HKL_MODIFIER) {
      hostSet.modifier = hostSet.modifier == SELECT ? -1 : SELECT;
      if (hostSet.modifier >= 0)
        modifier_takes(hostSet.modifier);
      ds_hotkeys_follow_modifier();
    } else
      set[row % HK_ACTIONS] = -1;
    host_save();
    hkLibCapture = -1;
    return;
  }
  for (int k = 0; k < NKEYS; k++)
    if (down(k) && (hotkey_assignable(k) || (row == HKL_MODIFIER && k == START))) {
      if (row == HKL_MODIFIER) {
        hostSet.modifier = k;
        modifier_takes(k);
        ds_hotkeys_follow_modifier();
      } else if (k == hostSet.modifier) {
        library_say("That button is the MODIFIER");
        hkLibCapture = -1;
        return;
      } else {
        for (int j = 0; j < HK_ACTIONS; j++)
          if (set[j] == k)
            set[j] = -1;
        set[row % HK_ACTIONS] = k;
      }
      host_save();
      hkLibCapture = -1;
      return;
    }
}

/* The game list's GAME SETTINGS page (a game's menu): that game's own LCD FILTER, FF SPEED, REWIND and A/B BUTTONS, each
 * ALL GAMES (as SETTINGS has it) or its own. A, a click, LEFT / RIGHT step the row. */
static char gsRows[GS_COUNT][56], gsGame[PATH_MAX * 2];
static const char *gsPtrs[GS_COUNT];
static int gsOwn[GS_COUNT];
static struct pd_menu gsMenu;
static void gs_value(int i, int v, char *out, size_t size) {
  if (i == GS_FF && v == 0)
    snprintf(out, size, "MAX");
  else if (i == GS_FF)
    snprintf(out, size, "%dx", v);
  else if (i == GS_SWAP)
    snprintf(out, size, "%s", v ? "SWAPPED" : "NORMAL");
  else
    snprintf(out, size, "%s", v ? "ON" : "OFF");
}
static void gs_build(void) {
  static const char *const names[GS_COUNT] = {"LCD FILTER", "FF SPEED", "REWIND", "A/B BUTTONS"};
  for (int i = 0; i < GS_COUNT; i++) {
    char v[16];
    if (gsOwn[i] >= 0)
      gs_value(i, gsOwn[i], v, sizeof v);
    else {
      char every[16];
      gs_value(i, gs_get(i), every, sizeof every);
      snprintf(v, sizeof v, "ALL (%s)", every);
    }
    snprintf(gsRows[i], sizeof gsRows[i], "%s  %s", names[i], v);
    gsPtrs[i] = gsRows[i];
  }
}
static void gs_open(const char *game) {
  snprintf(gsGame, sizeof gsGame, "%s", game);
  gs_read(game, gsOwn);
  gs_build();
  pd_menu_init(&gsMenu, "GAME SETTINGS", gsPtrs, GS_COUNT);
  gsMenu.overlay = true;
}
// ALL GAMES, then the row's own values in turn, round.
static void gs_step(int i, int dir) {
  static const int ffOrder[] = {-1, 0, 2, 3, 4, 5, 6, 7, 8}, onOff[] = {-1, 1, 0};
  const int *order = i == GS_FF ? ffOrder : onOff;
  const int n = i == GS_FF ? 9 : 3;
  int at = 0;
  for (int j = 0; j < n; j++)
    if (order[j] == gsOwn[i])
      at = j;
  gsOwn[i] = order[(at + dir + n) % n];
  gs_write(gsGame, gsOwn);
  gs_build();
}

/* ---- SAVES FOLDER (SETTINGS): where the battery saves live. The app's own folder, a cloud drive's folder (iCloud
 * Drive, Google Drive for desktop, a Syncthing folder) or one of the player's choosing. Moving copies what's there
 * without overwriting anything (a cloud folder that already has saves keeps them: they're another computer's). ---- */
enum { SV_APP, SV_ICLOUD, SV_GDRIVE, SV_SYNC, SV_CHOOSE, SV_SHOW, SV_ROWS };
static char svRows[SV_ROWS][64];
static const char *svPtrs[SV_ROWS];
static struct pd_menu svMenu;
static bool is_dir(const char *p) {
  struct stat st;
  return !stat(p, &st) && S_ISDIR(st.st_mode);
}
#ifndef _WIN32
static int run_argv(const char *const *argv) {
  pid_t pid;
  if (posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ))
    return -1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif
// The folder a row means (its "PokeDaisy Saves" folder in a cloud drive); false when that drive isn't on this computer.
static bool saves_folder_for(int which, char *out, size_t size) {
  const char *home = getenv("HOME");
  char base[PATH_MAX];
  base[0] = 0;
  if (which == SV_APP) {
    snprintf(out, size, "%s/saves", root);
    return true;
  }
  if (!home)
    return false;
  if (which == SV_ICLOUD)
    snprintf(base, sizeof base, "%s/Library/Mobile Documents/com~apple~CloudDocs", home);
  else if (which == SV_SYNC)
    snprintf(base, sizeof base, "%s/Sync", home);
#ifndef _WIN32
  else if (which == SV_GDRIVE) { // Google Drive for desktop: ~/Library/CloudStorage/GoogleDrive-<account>/My Drive
    char pattern[PATH_MAX];
    glob_t g;
    snprintf(pattern, sizeof pattern, "%s/Library/CloudStorage/GoogleDrive-*/My Drive", home);
    if (!glob(pattern, 0, NULL, &g) && g.gl_pathc)
      snprintf(base, sizeof base, "%s", g.gl_pathv[0]);
    globfree(&g);
  }
#endif
  if (!base[0] || !is_dir(base))
    return false;
  snprintf(out, size, "%s/PokeDaisy Saves", base);
  return true;
}
static void saves_folder_name(char *out, int len) {
  static const char *const names[] = {"APP FOLDER", "ICLOUD DRIVE", "GOOGLE DRIVE", "SYNCTHING"};
  char p[PATH_MAX];
  for (int i = SV_APP; i <= SV_SYNC; i++)
    if (saves_folder_for(i, p, sizeof p) && !strcmp(p, savesDir)) {
      snprintf(out, (size_t)len, "%s", names[i]);
      return;
    }
  const char *slash = strrchr(savesDir, '/');
  snprintf(out, (size_t)len, "%s", slash && slash[1] ? slash + 1 : savesDir);
  for (char *q = out; *q; q++)
    *q = (char)toupper((unsigned char)*q);
}
static void sv_build(void) {
  static const char *const names[] = {"APP FOLDER", "ICLOUD DRIVE", "GOOGLE DRIVE", "SYNCTHING FOLDER (~/Sync)"};
  static const char *const missing[] = {"", "  (NOT SET UP)", "  (NOT INSTALLED)", "  (NOT FOUND)"};
  char p[PATH_MAX];
  for (int i = SV_APP; i <= SV_SYNC; i++) {
    bool there = saves_folder_for(i, p, sizeof p);
    snprintf(svRows[i], sizeof svRows[i], "%s%s", names[i],
             !there ? missing[i] : !strcmp(p, savesDir) ? "  (IN USE)" : "");
  }
  snprintf(svRows[SV_CHOOSE], sizeof svRows[SV_CHOOSE], "CHOOSE A FOLDER...");
#ifdef __APPLE__
  snprintf(svRows[SV_SHOW], sizeof svRows[SV_SHOW], "SHOW SAVES IN FINDER");
#else
  snprintf(svRows[SV_SHOW], sizeof svRows[SV_SHOW], "SHOW SAVES FOLDER");
#endif
  for (int i = 0; i < SV_ROWS; i++)
    svPtrs[i] = svRows[i];
}
static void sv_open(void) {
  sv_build();
  pd_menu_init(&svMenu, "SAVES FOLDER", svPtrs, SV_ROWS);
  svMenu.overlay = true;
}
// The saves (and the cores' settings, the backups) copied into [to], nothing there overwritten; then it's the folder.
static void saves_move(const char *to) {
  if (!strcmp(to, savesDir)) {
    library_say("Your saves are already in that folder");
    return;
  }
#ifndef _WIN32
  const char *mk[] = {"mkdir", "-p", to, NULL};
  char from[PATH_MAX + 4], dest[PATH_MAX + 4];
  snprintf(from, sizeof from, "%s/.", savesDir);
  snprintf(dest, sizeof dest, "%s/", to);
  const char *cp[] = {"cp", "-R", "-n", from, dest, NULL};
  if (run_argv(mk) || !is_dir(to)) {
    library_say("That folder couldn't be made");
    return;
  }
  run_argv(cp); // cp -n says so (and fails) for each file it kept: that's what's wanted
#endif
  char app[PATH_MAX];
  saves_folder_for(SV_APP, app, sizeof app);
  snprintf(hostSet.savesFolder, sizeof hostSet.savesFolder, "%s", strcmp(to, app) ? to : "");
  snprintf(savesDir, sizeof savesDir, "%s", to);
  host_save();
  library_reload(); // the list looks for each game's save in the new folder
  char name[64], said[160];
  saves_folder_name(name, sizeof name);
  snprintf(said, sizeof said, "Saves now in %s (copied; any already there were kept)", name);
  library_say(said);
}
// A row picked: true when the page is done.
static bool sv_pick(int row) {
  char p[PATH_MAX];
  if (row <= SV_SYNC) {
    if (!saves_folder_for(row, p, sizeof p)) {
      library_say(row == SV_ICLOUD   ? "Turn on iCloud Drive in System Settings first"
                  : row == SV_GDRIVE ? "Install Google Drive for desktop and sign in first"
                                     : "Make a Sync folder in your home folder first (Syncthing's default)");
      return false;
    }
    saves_move(p);
    return true;
  }
#ifdef __APPLE__
  if (row == SV_CHOOSE) {
    FILE *f = popen("osascript -e 'POSIX path of (choose folder with prompt \"A folder for your PokeDaisy saves\")' "
                    "2>/dev/null", "r");
    char got[PATH_MAX] = "";
    if (f) {
      if (!fgets(got, sizeof got, f))
        got[0] = 0;
      pclose(f);
    }
    got[strcspn(got, "\r\n")] = 0;
    size_t n = strlen(got);
    if (n > 1 && got[n - 1] == '/')
      got[n - 1] = 0;
    if (got[0])
      saves_move(got);
    return got[0] != 0;
  }
  const char *show[] = {"open", savesDir, NULL};
  run_argv(show);
#elif !defined(_WIN32)
  if (row == SV_CHOOSE) {
    library_say("Pick a folder: put its path in host.ini as saves_dir=");
    return false;
  }
  const char *show[] = {"xdg-open", savesDir, NULL};
  run_argv(show);
#endif
  return true;
}

/* ---- RESTORE SAVE (a game's menu): the backups made of its save each time it's opened (newest first), one put back.
 * The save it replaces is backed up first, so a restore can be undone the same way. ---- */
#define RS_MAX 20
static char rsRows[RS_MAX][48], rsFiles[RS_MAX][256], rsSav[PATH_MAX * 2], rsBackups[PATH_MAX * 2];
static const char *rsPtrs[RS_MAX];
static int rsCount, rsArmed = -1;
static struct pd_menu rsMenu;
static void rs_open(const char *game) {
  pd_save_path(savesDir, game, rsSav, sizeof rsSav);
  snprintf(rsBackups, sizeof rsBackups, "%s/pokedaisy-backups", savesDir);
  const char *file = strrchr(rsSav, '/') ? strrchr(rsSav, '/') + 1 : rsSav, *dot = strrchr(file, '.');
  char stem[300];
  snprintf(stem, sizeof stem, "%.*s.backup-", dot && dot != file ? (int)(dot - file) : (int)strlen(file), file);
  char *list[256];
  int n = 0;
  DIR *d = opendir(rsBackups);
  for (struct dirent *e; d && (e = readdir(d)) && n < 256;)
    if (!strncmp(e->d_name, stem, strlen(stem)) && !strstr(e->d_name, ".tmp"))
      list[n++] = strdup(e->d_name);
  if (d)
    closedir(d);
  qsort(list, n, sizeof *list, newest_first);
  rsCount = 0;
  for (int i = 0; i < n; i++) {
    int y, mo, dd, h, mi;
    if (rsCount < RS_MAX && sscanf(list[i] + strlen(stem), "%4d%2d%2d-%2d%2d", &y, &mo, &dd, &h, &mi) == 5) {
      static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
      snprintf(rsRows[rsCount], sizeof rsRows[rsCount], "%s %d %d  %d:%02d %s", months[(mo + 11) % 12], dd, y,
               h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
      snprintf(rsFiles[rsCount], sizeof rsFiles[rsCount], "%s", list[i]);
      rsPtrs[rsCount] = rsRows[rsCount];
      rsCount++;
    }
    free(list[i]);
  }
  if (!rsCount) {
    snprintf(rsRows[0], sizeof rsRows[0], "NO BACKUPS YET");
    rsPtrs[0] = rsRows[0];
  }
  rsArmed = -1;
  pd_menu_init(&rsMenu, "RESTORE SAVE", rsPtrs, rsCount ? rsCount : 1);
  rsMenu.overlay = true;
}
// The first A on a backup asks, the second puts it back: true when the page is done.
static bool rs_pick(int row) {
  if (row < 0 || row >= rsCount)
    return !rsCount;
  if (rsArmed != row) {
    rsArmed = row;
    rsMenu.title = "A AGAIN: RESTORE THIS ONE";
    return false;
  }
  char from[PATH_MAX * 3];
  snprintf(from, sizeof from, "%s/%s", rsBackups, rsFiles[row]);
  size_t n;
  unsigned char *d = read_file(from, &n, 64u << 20);
  bool ok = d && save_backup(rsSav, rsBackups) && write_file(rsSav, d, n, true);
  free(d);
  library_say(ok ? "Save restored (the one it replaced is a backup now)" : "That backup couldn't be restored");
  library_reload();
  return true;
}

/* The library at a finer grain than the overlay (its 320x240 is the companion's): about 640x480 on a 1280x960 panel. Its
 * page, then the covers at the screen's own resolution, then a second canvas with the legend and the menus. */
static uint32_t libraryTopPx[OVERLAY_MAX_W * OVERLAY_MAX_H];
static SDL_Texture *libraryTopTexture, *coverTextures[1024];
static unsigned coverSerials[1024];
static int libraryTopW, libraryTopH;
/* The same viewport library_present draws: window pixels, the scale, the canvas and its top-left corner. */
static void library_viewport(int *ow, int *oh, int *scale, int *w, int *h, int *x0, int *y0) {
  int width = W, height = H;
  SDL_GetRendererOutputSize(renderer, &width, &height);
  int s = width / 560 < height / 400 ? width / 560 : height / 400;
  if (s < 1)
    s = 1;
  int cw = width / s, ch = height / s;
  cw = cw > OVERLAY_MAX_W ? OVERLAY_MAX_W : cw;
  ch = ch > OVERLAY_MAX_H ? OVERLAY_MAX_H : ch;
  *ow = width;
  *oh = height;
  *scale = s;
  *w = cw;
  *h = ch;
  *x0 = (width - cw * s) / 2;
  *y0 = (height - ch * s) / 2;
}
/* The KEYBOARD page over the game list: its place in the list's canvas (a 320 x 240 page, scaled up whole) and a window
 * click mapped into that page, for the page's rows. */
static int keyPageScale = 1, keyPageX, keyPageY;
static void key_page_place(int w, int h) {
  keyPageScale = w / 320 < h / 240 ? w / 320 : h / 240;
  keyPageScale = keyPageScale < 1 ? 1 : keyPageScale;
  keyPageX = (w - 320 * keyPageScale) / 2;
  keyPageY = (h - 240 * keyPageScale) / 2;
}
static bool key_page_pointer(int *px, int *py) {
  int ow, oh, s, w, h, x0, y0;
  library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
  key_page_place(w, h);
  int cx = (mouseWindowX - x0) / s, cy = (mouseWindowY - y0) / s;
  if (cx < keyPageX || cy < keyPageY || cx >= keyPageX + 320 * keyPageScale || cy >= keyPageY + 240 * keyPageScale)
    return false;
  *px = (cx - keyPageX) / keyPageScale;
  *py = (cy - keyPageY) / keyPageScale;
  return true;
}
static void library_textures_free(void) {
  for (int i = 0; i < 1024; i++)
    if (coverTextures[i]) {
      SDL_DestroyTexture(coverTextures[i]);
      coverTextures[i] = NULL;
    }
  if (libraryTopTexture)
    SDL_DestroyTexture(libraryTopTexture);
  libraryTopTexture = NULL;
}
static void library_present(struct pd_menu *page) {
  int ow, oh, s, w, h, x0, y0;
  library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
  SDL_RenderClear(renderer);
  pd_canvas_init(&overlay, w, h, overlayPx);
  library_draw(&overlay);
  if (page) {
    // The KEYBOARD page over the list, the list dimmed behind it as the game list's own windows dim it. The page is
    // drawn at the host menu's own density (320x240) and scaled up whole, so its text is as large as in a game.
    pd_unclip(&overlay); // the list's own drawing may have left a clip: the dim covers the whole picture
    pd_dim(&overlay, 150);
    static uint32_t pagePx[320 * 240];
    struct pd_canvas pc;
    pd_canvas_init(&pc, 320, 240, pagePx);
    pd_fill(&pc, 0, 0, 320, 240, KEY_COLOR);
    pd_menu_draw(page, &pc);
    key_page_place(overlay.w, overlay.h);
    const int sc = keyPageScale, ox = keyPageX, oy = keyPageY;
    for (int y = 0; y < 240 * sc; y++)
      for (int x = 0; x < 320 * sc; x++) {
        uint32_t p = pagePx[(y / sc) * 320 + x / sc] & 0xFFFFFFu;
        const int dx = ox + x, dy = oy + y; // a window smaller than the page cuts it, never writes past the canvas
        if (p != (KEY_COLOR & 0xFFFFFFu) && dx >= 0 && dy >= 0 && dx < overlay.w && dy < overlay.h)
          overlay.px[dy * overlay.w + dx] = 0xFF000000u | p;
      }
  }
  overlay_draw(true);
  if (page) { // no legend or rail over the page
    SDL_RenderPresent(renderer);
    return;
  }
  struct lib_cover covers[64];
  int n = library_covers(covers, 64);
  for (int i = 0; i < n; i++) {
    const struct lib_cover *cv = &covers[i];
    if (cv->game < 0 || cv->game >= 1024)
      continue;
    SDL_Texture **t = &coverTextures[cv->game];
    if (!*t || coverSerials[cv->game] != cv->serial + 1) {
      if (!*t) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, cv->size, cv->size);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        if (*t)
          SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_BLEND);
      }
      if (*t)
        SDL_UpdateTexture(*t, NULL, cv->px, cv->size * 4);
      coverSerials[cv->game] = cv->serial + 1;
    }
    if (!*t)
      continue;
    SDL_Rect clip = {x0 + cv->clipX * s, y0 + cv->clipY * s, cv->clipW * s, cv->clipH * s};
    SDL_Rect dst = {x0 + cv->x * s, y0 + cv->y * s, cv->w * s, cv->h * s};
    SDL_RenderSetClipRect(renderer, &clip);
    SDL_RenderCopy(renderer, *t, NULL, &dst);
    SDL_RenderSetClipRect(renderer, NULL);
  }
  if (library_dimmed()) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_RenderFillRect(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  }
  struct pd_canvas top;
  pd_canvas_init(&top, w, h, libraryTopPx);
  library_draw_top(&top, KEY_COLOR);
  if (!libraryTopTexture || libraryTopW != w || libraryTopH != h) {
    if (libraryTopTexture)
      SDL_DestroyTexture(libraryTopTexture);
    libraryTopTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (libraryTopTexture)
      SDL_SetTextureBlendMode(libraryTopTexture, SDL_BLENDMODE_BLEND);
    libraryTopW = w;
    libraryTopH = h;
  }
  for (int i = 0; i < w * h; i++) {
    uint32_t p = libraryTopPx[i] & 0xFFFFFF;
    libraryTopPx[i] = p == KEY_COLOR ? 0 : 0xFF000000u | p;
  }
  if (libraryTopTexture) {
    SDL_UpdateTexture(libraryTopTexture, NULL, libraryTopPx, w * 4);
    SDL_Rect r = {x0, y0, w * s, h * s};
    SDL_RenderCopy(renderer, libraryTopTexture, NULL, &r);
  }
  SDL_RenderPresent(renderer);
}
/* By file name; the same file in two folders: DATA/roms' first (the library keeps the first), then by path. */
static bool in_data_roms(const char *p) {
  size_t n = strlen(root);
  return !strncmp(p, root, n) && !strncmp(p + n, "/roms/", 6);
}
static int cmp(const void *a, const void *b) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  int c = strcasecmp(base_name(x), base_name(y));
  if (!c)
    c = strcmp(base_name(x), base_name(y));
  if (!c && in_data_roms(x) != in_data_roms(y))
    c = in_data_roms(x) ? -1 : 1;
  return c ? c : strcmp(x, y);
}
static void exe_dir(const char *argv0) {
  char p[PATH_MAX];
#ifdef _WIN32
  long n = pd_exe_path(p, sizeof p); // GetModuleFileNameA: Windows has no /proc/self/exe
#elif defined(__APPLE__)
  uint32_t size = sizeof p; // macOS has no /proc either
  char raw[PATH_MAX];
  long n = _NSGetExecutablePath(raw, &size) == 0 && realpath(raw, p) ? (long)strlen(p) : -1;
#else
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
#endif
  if (n > 0)
    p[n] = 0;
  else
    snprintf(p, sizeof p, "%s", argv0);
  char *slash = strrchr(p, '/');
  if (slash)
    *slash = 0;
  else
    snprintf(p, sizeof p, ".");
#ifdef _WIN32
  for (char *c = p; *c; c++) // one separator throughout: base_name and the path joins below use '/'
    if (*c == '\\')
      *c = '/';
#endif
  snprintf(exeDir, sizeof exeDir, "%s", p);
}
int main(int argc, char **argv) {
  const char *rom = NULL, *shot = NULL;
  int frames = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data") && i + 1 < argc)
      root = argv[++i];
    else if (!strcmp(argv[i], "--save") && i + 1 < argc)
      saveFile = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
      shot = argv[++i];
    else
      rom = argv[i];
  }
  exe_dir(argv[0]);
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
#ifdef SIGHUP // a closed terminal or session: the save goes to the card like a quit (no SIGHUP on Windows)
  signal(SIGHUP, stop);
#endif
  pd_mkdir(root);
  char path[PATH_MAX * 2];
  const char *dirs[] = {"roms", "saves", "states"};
  for (int i = 0; i < 3; i++) {
    snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
    pd_mkdir(path);
  }
  snprintf(savesDir, sizeof savesDir, "%s/saves", root);
  snprintf(hostIni, sizeof hostIni, "%s/host.ini", root);
  host_load();
  struct stat savesSt; // SAVES FOLDER: the one picked, while it's there (a cloud folder signed out of: the app's own)
  if (hostSet.savesFolder[0] && !stat(hostSet.savesFolder, &savesSt) && S_ISDIR(savesSt.st_mode))
    snprintf(savesDir, sizeof savesDir, "%s", hostSet.savesFolder);
  else if (hostSet.savesFolder[0])
    fprintf(stderr, "Saves folder %s isn't there: using %s\n", hostSet.savesFolder, savesDir);
  faceLabels = getenv("POCKET_FACE_LABELS") && !strcmp(getenv("POCKET_FACE_LABELS"), "1");
  comboMenu = !(getenv("POCKET_MENU_COMBO") && !strcmp(getenv("POCKET_MENU_COMBO"), "off"));
  // A window manager can give another window the focus over the game (plorpOS's in-game menu on the GKD): the
  // controller has to keep reaching the host then.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __APPLE__
  // Xbox pads through macOS's GameController framework, not SDL's own HIDAPI driver: the HIDAPI driver opens a
  // virtual Xbox pad (an iPhone controller app's, for one) but receives none of its input. macOS reads real ones too.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_DisplayMode display = {0};
  // Fullscreen with POCKET_FULLSCREEN, and under gamescope (the Steam Deck's Game Mode): it scales a window to the
  // screen keeping its shape, so a window there was letterboxed and EXPAND couldn't reach the edges.
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  bool fullscreen = hostSet.fullscreen || getenv("POCKET_FULLSCREEN") != NULL || (desktop && strstr(desktop, "gamescope"));
  if (fullscreen && SDL_GetCurrentDisplayMode(0, &display))
    fullscreen = false;
  // POCKET_WINDOW=WxH: a window the size of a device's panel, for trying a layout on a desktop.
  int winW = W, winH = H;
  const char *size = getenv("POCKET_WINDOW");
  if (size &&
      (sscanf(size, "%dx%d", &winW, &winH) != 2 || winW < 160 || winH < 144 || winW > 8192 || winH > 8192))
    winW = W, winH = H;
  // The window where it was last time, if that's still on a screen.
  int winX = SDL_WINDOWPOS_CENTERED, winY = SDL_WINDOWPOS_CENTERED;
  if (!size && hostSet.winW) {
    winW = hostSet.winW, winH = hostSet.winH;
    for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
      SDL_Rect b;
      if (!SDL_GetDisplayBounds(d, &b) && hostSet.winX + 40 >= b.x && hostSet.winX + 40 < b.x + b.w &&
          hostSet.winY + 20 >= b.y && hostSet.winY + 20 < b.y + b.h)
        winX = hostSet.winX, winY = hostSet.winY;
    }
  }
  window = SDL_CreateWindow(PD_HOST_NAME, winX, winY,
                            fullscreen ? display.w : winW, fullscreen ? display.h : winH,
                            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
#ifdef _WIN32
  set_window_icon(window);
#endif
#ifdef __APPLE__
  {
    static const int menuCodes[] = {CMD_SETTINGS,  CMD_SEARCH,     CMD_BIGGER,     CMD_SMALLER,
                                    CMD_ACTUAL,    CMD_FULLSCREEN, CMD_GAME_MENU,  CMD_SAVE_STATE,
                                    CMD_LOAD_STATE, CMD_SCREENSHOT, CMD_GAMES_FOLDER, CMD_RELEASES};
    mac_menu_install(menu_add_game, menuCodes);
  }
#endif
  // Presents wait for the refresh (the pacing, above); a driver without it gets the timer.
  renderer = window ? SDL_CreateRenderer(window, -1, getenv("POCKET_NO_VSYNC") ? 0 : SDL_RENDERER_PRESENTVSYNC) : NULL;
  if (!renderer && window)
    renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    fprintf(stderr, "Video: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_RendererInfo rendererInfo;
  if (!SDL_GetRendererInfo(renderer, &rendererInfo))
    fprintf(stderr, "Video backend: %s\n", rendererInfo.name);
#ifdef _WIN32
  // The mouse taps the library and the app's own overlays: its cursor stays on screen.
  SDL_ShowCursor(SDL_ENABLE);
#else
  // A handheld (KMSDRM, gamescope) would draw an arrow over the game: hidden until the mouse moves (events_poll).
  SDL_ShowCursor(SDL_DISABLE);
#endif
  // SDL starts text input with the window, and Steam answers it with its on-screen keyboard: the host reads keys,
  // never text.
  SDL_StopTextInput();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  audio_open();
  rawTrimui = getenv("POCKET_RAW_TRIMUI") && !strcmp(getenv("POCKET_RAW_TRIMUI"), "1");
#ifdef __APPLE__
  // Ekipad (an iPhone as a Mac controller) in its Generic Gamepad route: a virtual HID pad SDL has no mapping for.
  // 13 buttons (A B X Y L1 R1 L2 R2 L3 R3 START SELECT HOME), a hat, sticks on X/Y and Z/Rz, triggers on Rx/Ry.
  SDL_GameControllerAddMapping("0300164309120000ade4000000000000,Ekipad Pad,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,"
                               "rightshoulder:b5,lefttrigger:a3,righttrigger:a4,leftstick:b8,rightstick:b9,start:b10,"
                               "back:b11,guide:b12,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,"
                               "rightx:a2,righty:a5,platform:Mac OS X,");
#endif
  snprintf(path, sizeof path, "%s/gamecontrollerdb.txt", root);
  if (access(path, R_OK) == 0) {
    int added = SDL_GameControllerAddMappingsFromFile(path);
    fprintf(stderr, "Controller mappings from %s: %d\n", path, added);
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    if (SDL_IsGameController(i))
      controller_added(i);
  SDL_Joystick *js = !controller && rawTrimui && SDL_NumJoysticks() ? SDL_JoystickOpen(0) : NULL;
  if (rom)
    run_game(rom, frames, shot, false);
  else {
    /* DATA/roms, then the folders POCKET_ROM_DIRS names (colon-separated: the PortMaster launcher passes the
     * firmware's own gba / gb / gbc / nds folders), by file name. Those hold every other game too, so only their
     * Pokemon games are listed (pokemon_rom); DATA/roms lists whatever was put there. */
  list_again:; // games were added from the list: list the folders again
    static char *paths[1024];
    int count = 0;
    char dirs[4096];
    snprintf(dirs, sizeof dirs, "%s/roms%s%s", root, getenv("POCKET_ROM_DIRS") ? ":" : "",
             getenv("POCKET_ROM_DIRS") ? getenv("POCKET_ROM_DIRS") : "");
    char *save = NULL;
    bool firmware = false;
    for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save), firmware = true) {
      DIR *d = opendir(dir);
      struct dirent *e;
      while (d && (e = readdir(d)) && count < 1024) {
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (e->d_name[0] != '.' && core_for(path)) { // by the whole path: a zip is opened to see what's inside
          if (!firmware || pokemon_rom(path))
            paths[count++] = strdup(path);
        }
      }
      if (d)
        closedir(d);
    }
    qsort(paths, count, sizeof(*paths), cmp);
    settings_migrate(); // before the library writes its first settings: an old DATA/settings.ini wins
    library_open(paths, count, root, savesDir, &libraryHost);
    int ticks = 0;
    bool keyPage = false; // the KEYBOARD page over the game list
    bool hkPage = false, gsPage = false; // HOTKEYS (SETTINGS), GAME SETTINGS (a game's menu)
    bool svPage = false, rsPage = false; // SAVES FOLDER (SETTINGS), RESTORE SAVE (a game's menu)
    while (running && !library_quit() && !library_reload_wanted()) {
      listShown = true;
      events();
      repeat_directions();
      static const int LIB_KEYS[][2] = {{A, LIB_A},       {B, LIB_B},         {X, LIB_X},           {Y, LIB_Y},
                                        {L, LIB_L},       {R, LIB_R},         {UP, LIB_UP},         {DOWN, LIB_DOWN},
                                        {LEFT, LIB_LEFT}, {RIGHT, LIB_RIGHT}, {START, LIB_START},   {SELECT, LIB_SELECT},
                                        {MENU, LIB_MENU}};
      unsigned libDown = 0;
      for (size_t i = 0; i < sizeof LIB_KEYS / sizeof *LIB_KEYS; i++)
        if (down(LIB_KEYS[i][0]))
          libDown |= (unsigned)LIB_KEYS[i][1];
      // START + SELECT (dev's quit chord) asks to close as START or Menu do.
      if ((down(START) && (held >> SELECT & 1)) || (down(SELECT) && (held >> START & 1)))
        libDown = (libDown & ~(unsigned)LIB_SELECT) | LIB_START;
      if (keyPage || hkPage || gsPage || svPage || rsPage) // the page has the keys
        libDown = 0;
      library_keys(libDown);
      library_tick();
      if (libraryKeyboard) {
        libraryKeyboard = false;
        keyPage = true;
        kbCapture = -1;
        kb_menu_open();
        pressed = held = 0; // the A that opened the page isn't the page's own
        libraryClick = false; // nor is the click that did
      }
      if (libraryHotkeys || librarySaves || library_game_settings()) {
        if (libraryHotkeys)
          hk_lib_open(), hkPage = true;
        else if (librarySaves)
          sv_open(), svPage = true;
        else if (library_game_page() == LIB_PAGE_RESTORE)
          rs_open(library_game_settings()), rsPage = true;
        else
          gs_open(library_game_settings()), gsPage = true;
        libraryHotkeys = librarySaves = false;
        library_game_settings_done();
        pressed = held = 0;
        libraryClick = false;
      }
      if (hkPage || gsPage || svPage || rsPage) {
        struct pd_menu *m = hkPage ? &hkLibMenu : gsPage ? &gsMenu : svPage ? &svMenu : &rsMenu;
        int px = 0, py = 0, row = -1;
        bool onPage = libraryClick && key_page_pointer(&px, &py);
        if (onPage) { // a click on a row: as A on it
          pd_menu_touch(m, px, py, true);
          row = pd_menu_touch(m, px, py, false);
        }
        if (hkPage && hkLibCapture >= 0) {
          hk_lib_capture();
          if (hkLibCapture >= 0 && libraryClick && !onPage)
            hkLibCapture = -1;
        } else if (down(B) || down(MENU) || (libraryClick && !onPage)) {
          hkPage = gsPage = svPage = rsPage = false; // B, Escape or a click beside the page closes it
        } else {
          if (down(UP))
            pd_menu_move(m, -1);
          if (down(DOWN))
            pd_menu_move(m, 1);
          if (row >= 0)
            m->selected = row;
          if (hkPage && (down(A) || row >= 0)) {
            hkLibCapture = m->selected;
            pressed = held = 0; // the button that picked the row isn't its new one
          } else if (gsPage && (down(A) || down(RIGHT) || row >= 0))
            gs_step(m->selected, 1);
          else if (gsPage && down(LEFT))
            gs_step(m->selected, -1);
          else if (svPage && (down(A) || row >= 0)) {
            if (sv_pick(m->selected))
              svPage = false;
            else
              sv_build();
          } else if (rsPage && (down(A) || row >= 0)) {
            if (rs_pick(m->selected))
              rsPage = false;
          } else if (rsPage && (down(UP) || down(DOWN)) && rsArmed >= 0) {
            rsArmed = -1; // moved off the backup asked about: ask again
            rsMenu.title = "RESTORE SAVE";
          }
        }
        if (hkPage)
          hk_lib_build();
        libraryClick = libraryRightClick = false;
        libraryWheel = 0;
      } else if (keyPage) {
        int px = 0, py = 0;
        bool onPage = libraryClick && key_page_pointer(&px, &py);
        if (kbCapture >= 0) {
          // The key that went down is the row's (taken from any other row that had it); SELECT turns it off.
          if (lastKey == SDLK_ESCAPE)
            lastKey = 0, pressed |= 1u << B;
          else if (lastKey == SDLK_BACKSPACE || lastKey == SDLK_DELETE)
            lastKey = 0, pressed |= 1u << SELECT;
          if (lastKey) {
            for (int i = 0; i < KB_COUNT; i++)
              if (kbKeys[i] == lastKey)
                kbKeys[i] = SDLK_UNKNOWN;
            kbKeys[kbCapture] = lastKey;
            host_save();
            kbCapture = -1;
            kbMenu.title = "KEYBOARD";
          } else if (down(B)) { // a controller's B cancels; a keyboard's keys are the binding
            kbCapture = -1;
            kbMenu.title = "KEYBOARD";
          } else if (down(SELECT)) {
            kbKeys[kbCapture] = SDLK_UNKNOWN;
            host_save();
            kbCapture = -1;
            kbMenu.title = "KEYBOARD";
          } else if (libraryClick && !onPage) { // a click beside the page closes it, as the app's floating touch screen does
            keyPage = false, kbCapture = -1, kbMenu.title = "KEYBOARD";
          }
        } else if (down(B) || down(MENU)) { // B or Escape closes the page
          keyPage = false, kbCapture = -1, kbMenu.title = "KEYBOARD";
        } else if (down(A)) {
          kbCapture = kbMenu.selected;
          pressed = held = 0;
          kbMenu.title = "PRESS A KEY";
        } else if (onPage) {
          // A click on a row picks it and waits for the key, as A does on the cursor.
          pd_menu_touch(&kbMenu, px, py, true);
          const int row = pd_menu_touch(&kbMenu, px, py, false);
          if (row >= 0) {
            kbMenu.selected = row;
            kbCapture = row;
            pressed = held = 0;
            kbMenu.title = "PRESS A KEY";
          }
        } else if (libraryClick) {
          keyPage = false; // a click beside the page closes it
        }
        kb_menu_build();
        libraryClick = libraryRightClick = false;
        libraryWheel = 0;
      } else {
        int ow, oh, s, w, h, x0, y0;
        library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
        int mx = (mouseWindowX - x0) / s, my = (mouseWindowY - y0) / s;
        library_hold(mx, my, pointerHeld); // a scroll bar being dragged follows the pointer
        if (libraryDropped[0]) {
          library_drop(libraryDropped);
          libraryDropped[0] = 0;
        }
        for (int i = 0; i < addQueued; i++) // File > Add Games...
          library_drop(addQueue[i]);
        addQueued = 0;
        if ((libraryClick || libraryRightClick || libraryWheel) && mx >= 0 && my >= 0 && mx < w && my < h) {
          if (libraryRightClick)
            library_context(mx, my);
          else
            library_pointer(mx, my, libraryClick, libraryWheel);
        }
        libraryClick = libraryRightClick = false;
        libraryWheel = 0;
      }
      const char *game = library_picked();
      if (game) {
        char picked[PATH_MAX];
        snprintf(picked, sizeof picked, "%s", game);
        appError = 0;
        library_pause(true); // no hashing or cover downloads while a game plays
        run_game(picked, 0, NULL, true); // the last game's result is the exit status
        libraryDropped[0] = 0; // something dropped during the game wasn't meant as a cover
        library_pause(false);
        libraryClick = false; // a click in the game is not one in the library
        libraryWheel = 0;
        library_played(picked);
      }
      library_present(keyPage ? &kbMenu : hkPage ? &hkLibMenu : gsPage ? &gsMenu : svPage ? &svMenu : rsPage ? &rsMenu : NULL);
      if (frames && ++ticks >= frames) {
        if (shot && !capture(shot))
          appError = 1;
        break;
      }
      SDL_Delay(16);
    }
    bool again = running && !library_quit() && library_reload_wanted();
    library_close();
    library_textures_free();
    for (int i = 0; i < count; i++)
      free(paths[i]);
    if (again)
      goto list_again;
  }
  if (js)
    SDL_JoystickClose(js);
  while (padCount)
    SDL_GameControllerClose(pads[--padCount]);
  controller = NULL;
  if (audio)
    SDL_CloseAudioDevice(audio);
  if (frameTexture)
    SDL_DestroyTexture(frameTexture);
  if (overlayTexture)
    SDL_DestroyTexture(overlayTexture);
  if (lcdTexture)
    SDL_DestroyTexture(lcdTexture);
  SDL_DestroyRenderer(renderer);
  if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && !getenv("POCKET_WINDOW")) { // its place, for next time
    SDL_GetWindowPosition(window, &hostSet.winX, &hostSet.winY);
    SDL_GetWindowSize(window, &hostSet.winW, &hostSet.winH);
    host_save();
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return appError;
}
