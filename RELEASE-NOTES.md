# Sonic Screenlocker Release Notes

Sonic Screenlocker keeps your SonicDE session safe when you step away, locking the screen and showing the unlock screen where you sign back in.

## Unreleased

- The unlock screen now uses SonicDE's own on-screen keyboard, and the lock screen no longer covers it up.
- The unlock screen now follows your chosen theme for a consistent look.
- The on-screen keyboard's language list now scrolls properly, and its settings button is hidden on the unlock screen, where it can't be used.
- The screen locker now talks to its unlock screen through SonicDE's new fast, robust communication framework.
- The screen locker, unlock screen and password checker now log through SonicDE's new unified logging system, with a separate log for each sign-in method (password, fingerprint, smart card and so on).
- The screen locker now consistently identifies itself with SonicDE names.

## 6.8.0.2 (2026-10-25)

- The unlock screen now follows your chosen theme for a consistent look.
- The on-screen keyboard's language list now scrolls properly, and its settings button is hidden on the unlock screen, where it can't be used.

## 6.8.0.1 (2026-10-23)

- The unlock screen now uses SonicDE's own on-screen keyboard, and the lock screen no longer covers it up.

## 6.8.0 (2026-10-20)

- After a wrong password, the unlock screen now properly respects the system's security delay before you can try again.
- The unlock screen now uses the same footer as the rest of SonicDE's sign-in screens.
- Behind the scenes: new automated checks for password handling and the 6.8.0 release update.

## 6.7.5.1 (2026-09-25)

- After a wrong password, the unlock screen now properly respects the system's security delay before you can try again.

## 6.7.5 (2026-09-12)

- Behind the scenes: the 6.7.5 release update.

## 6.7.4 (2026-08-04)

No SonicDE-specific changes in this release; it brings the package up to date with upstream.

## 6.7.3.2 (2026-08-02)

- Behind the scenes: code formatting now follows SonicDE's shared settings, and a fix that lets other SonicDE components build against the screen locker.

## 6.7.3.1 (2026-07-30)

- Screen locker log files now live in your personal state folder and are readable only by you, keeping them private.

## 6.7.3 (2026-07-14)

- Behind the scenes: code formatting rules and developer editor support.

## 6.7.2 (2026-06-23)

No SonicDE-specific changes in this release; it brings the package up to date with upstream.

## 6.7.1 (2026-06-23)

No SonicDE-specific changes in this release; it brings the package up to date with upstream.

## 6.7.0 (2026-06-20)

- The screen locker's logging was rebuilt to work like the Sonic Login Manager's, with clearer, tidier log messages.

## 6.6.5.2 (2026-05-21)

- The screen locker and its unlock screen now talk to each other through a dedicated private channel, making unlocking more dependable.
- The unlock screen now appears reliably on every screen, including monitors that are connected or removed while locked.
- Keyboard focus handling on the unlock screen is now more precise.
- The screen locker now keeps its own log files when systemd's journal isn't available, starting a fresh log each time.
- Fixed problems on systems where the global shortcuts service isn't running.
- Behind the scenes: log message filtering, compiler warning fixes and build cleanups.

## 6.6.5.1 (2026-05-17)

- Fixed the unlock screen sometimes appearing behind other windows or not accepting keyboard input.

## 6.6.5 (2026-05-12)

No SonicDE-specific changes in this release; it brings the package up to date with upstream.

## 6.6.4 (2026-04-08)

- The password field now gets keyboard focus back automatically when you return to the unlock screen or move the mouse to another monitor.

## 6.6.3.4 (2026-03-27)

- Fixed the unlock screen not responding properly when woken with the keyboard.
- The screen locker now runs purely on X11, with no Wayland dependency, and reliably tracks its unlock screen windows.
- Behind the scenes: build fixes, developer editor support and a low-level code safety fix.

## 6.6.3.3 (2026-03-22)

- The screen locker now reliably tracks its unlock screen windows on X11.
- Behind the scenes: a low-level code safety fix.

## 6.6.3.2 (2026-03-22)

- The screen locker now reliably tracks its unlock screen windows on X11, replacing a mechanism that only worked on Wayland.
- Behind the scenes: build cleanups.

## 6.6.3.1 (2026-03-21)

- The screen locker no longer relies on a Wayland-only windowing library.
- Behind the scenes: developer editor support.

## 6.6.3 (2026-03-17)

- The screen locker no longer depends on Wayland.
- Behind the scenes: a build fix for where the screen locker's developer files are installed.
