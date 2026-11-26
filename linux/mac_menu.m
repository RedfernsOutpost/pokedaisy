/* The Mac's menu bar for the desktop host (mac_menu.h). SDL makes the app menu and Window; these go between them. */
#import <Cocoa/Cocoa.h>

#include <SDL.h>

#include "mac_menu.h"

// codes[] order: the host's CMD_ values for these, as mac_menu_install's caller lists them.
enum { M_SETTINGS, M_SEARCH, M_BIGGER, M_SMALLER, M_ACTUAL, M_FULLSCREEN, M_GAME_MENU, M_SAVE, M_LOAD, M_SHOT,
       M_FOLDER, M_RELEASES, M_COUNT };
static int codeFor[M_COUNT];
static void (*addGame)(const char *path);

@interface PDMenuTarget : NSObject
@end
@implementation PDMenuTarget
- (void)command:(NSMenuItem *)item {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_USEREVENT;
  e.user.code = (Sint32)item.tag;
  SDL_PushEvent(&e);
}
- (void)addGames:(NSMenuItem *)item {
  (void)item;
  NSOpenPanel *panel = [NSOpenPanel openPanel];
  panel.allowsMultipleSelection = YES;
  panel.canChooseDirectories = NO;
  panel.message = @"Game files to add to Daisy Launcher (.gb, .gbc, .gba, .nds or .zip)";
  panel.prompt = @"Add";
  if ([panel runModal] != NSModalResponseOK || !addGame)
    return;
  for (NSURL *url in panel.URLs)
    addGame(url.fileSystemRepresentation);
}
@end

static PDMenuTarget *target;

static NSMenuItem *entry(NSString *title, NSString *key, NSEventModifierFlags mods, int which, SEL action) {
  NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  it.keyEquivalentModifierMask = mods;
  it.target = target;
  it.tag = which >= 0 ? codeFor[which] : 0;
  return it;
}
static NSMenuItem *command(NSString *title, NSString *key, NSEventModifierFlags mods, int which) {
  return entry(title, key, mods, which, @selector(command:));
}
static void menu_insert(NSMenu *bar, NSMenu *menu, NSInteger at) {
  NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:menu.title action:nil keyEquivalent:@""];
  holder.submenu = menu;
  [bar insertItem:holder atIndex:at < bar.numberOfItems ? at : bar.numberOfItems];
}
// Whether any item in the bar already answers this key (SDL's own Full Screen, say).
static bool taken(NSMenu *menu, NSString *key, NSEventModifierFlags mods) {
  for (NSMenuItem *it in menu.itemArray) {
    if (it.submenu && taken(it.submenu, key, mods))
      return true;
    if ([it.keyEquivalent isEqualToString:key] && (it.keyEquivalentModifierMask & mods) == mods)
      return true;
  }
  return false;
}

void mac_menu_install(void (*add_game)(const char *path), const int *codes) {
  NSMenu *bar = NSApp.mainMenu;
  if (!bar || target)
    return;
  target = [PDMenuTarget new];
  addGame = add_game;
  for (int i = 0; i < M_COUNT; i++)
    codeFor[i] = codes[i];
  const NSEventModifierFlags cmd = NSEventModifierFlagCommand;

  // The app menu: SDL's Preferences... (Cmd+,) becomes the game list's SETTINGS.
  NSMenu *app = bar.numberOfItems ? [bar itemAtIndex:0].submenu : nil;
  bool settings = false;
  for (NSMenuItem *it in app.itemArray)
    if ([it.keyEquivalent isEqualToString:@","]) {
      it.title = @"Settings…";
      it.target = target;
      it.action = @selector(command:);
      it.tag = codeFor[M_SETTINGS];
      settings = true;
    }
  if (app && !settings) {
    [app insertItem:[NSMenuItem separatorItem] atIndex:app.numberOfItems > 1 ? 1 : app.numberOfItems];
    [app insertItem:command(@"Settings…", @",", cmd, M_SETTINGS) atIndex:app.numberOfItems > 2 ? 2 : app.numberOfItems];
  }

  NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
  [file addItem:entry(@"Add Games…", @"o", cmd, -1, @selector(addGames:))];
  [file addItem:command(@"Show Games Folder", @"", 0, M_FOLDER)];

  NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
  [view addItem:command(@"Search Games", @"f", cmd, M_SEARCH)];
  [view addItem:[NSMenuItem separatorItem]];
  [view addItem:command(@"Bigger Covers", @"=", cmd, M_BIGGER)];
  [view addItem:command(@"Smaller Covers", @"-", cmd, M_SMALLER)];
  [view addItem:command(@"Usual Cover Size", @"0", cmd, M_ACTUAL)];
  if (!taken(bar, @"f", cmd | NSEventModifierFlagControl)) {
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:command(@"Full Screen", @"f", cmd | NSEventModifierFlagControl, M_FULLSCREEN)];
  }

  NSMenu *game = [[NSMenu alloc] initWithTitle:@"Game"];
  [game addItem:command(@"Game Menu", @"", 0, M_GAME_MENU)];
  [game addItem:[NSMenuItem separatorItem]];
  [game addItem:command(@"Save State", @"", 0, M_SAVE)];
  [game addItem:command(@"Load State", @"", 0, M_LOAD)];
  [game addItem:[NSMenuItem separatorItem]];
  [game addItem:command(@"Screenshot", @"s", cmd, M_SHOT)];

  NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
  [help addItem:command(@"Daisy Launcher Downloads", @"", 0, M_RELEASES)];

  menu_insert(bar, file, 1);
  menu_insert(bar, view, 2);
  menu_insert(bar, game, 3);
  menu_insert(bar, help, bar.numberOfItems);
}

// ---- the updater's Mac parts (mac_menu.h) ----
#import <Security/Security.h>

bool mac_bundle_path(char *out, size_t len) {
  NSString *path = NSBundle.mainBundle.bundlePath;
  if (!path || ![path.pathExtension isEqualToString:@"app"])
    return false;
  return snprintf(out, len, "%s", path.fileSystemRepresentation) < (int)len;
}

static NSString *self_team(void) {
  SecCodeRef me = NULL;
  CFDictionaryRef info = NULL;
  NSString *team = nil;
  if (SecCodeCopySelf(kSecCSDefaultFlags, &me) == errSecSuccess) {
    SecStaticCodeRef still = NULL;
    if (SecCodeCopyStaticCode(me, kSecCSDefaultFlags, &still) == errSecSuccess) {
      if (SecCodeCopySigningInformation(still, kSecCSSigningInformation, &info) == errSecSuccess && info) {
        id t = ((__bridge NSDictionary *)info)[(__bridge NSString *)kSecCodeInfoTeamIdentifier];
        if ([t isKindOfClass:NSString.class] && [t length])
          team = [t copy];
        CFRelease(info);
      }
      CFRelease(still);
    }
    CFRelease(me);
  }
  return team;
}

bool mac_same_signer(const char *app) {
  NSString *team = self_team();
  if (!team)
    return false;
  NSString *req = [NSString stringWithFormat:@"anchor apple generic and certificate leaf[subject.OU] = \"%@\"", team];
  SecRequirementRef requirement = NULL;
  SecStaticCodeRef code = NULL;
  bool ok = false;
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:app]];
  if (SecRequirementCreateWithString((__bridge CFStringRef)req, kSecCSDefaultFlags, &requirement) == errSecSuccess &&
      SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &code) == errSecSuccess)
    ok = SecStaticCodeCheckValidity(code, kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode,
                                    requirement) == errSecSuccess;
  if (code)
    CFRelease(code);
  if (requirement)
    CFRelease(requirement);
  return ok;
}

bool mac_swap_app(const char *current, const char *fresh) {
  NSFileManager *fm = NSFileManager.defaultManager;
  NSURL *cur = [NSURL fileURLWithPath:[NSString stringWithUTF8String:current]];
  NSURL *neu = [NSURL fileURLWithPath:[NSString stringWithUTF8String:fresh]];
  NSURL *trashed = nil;
  if (![fm trashItemAtURL:cur resultingItemURL:&trashed error:nil] || !trashed)
    return false;
  if ([fm moveItemAtURL:neu toURL:cur error:nil])
    return true;
  [fm moveItemAtURL:trashed toURL:cur error:nil]; // back where it was
  return false;
}
