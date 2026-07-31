[English](00-getting-started.md) | [Русский](00-getting-started_RU.md)

# Getting started with tesmioloader

This is a plain-language guide for players, not modders. If you just want to
turn features on, tweak a number, and see it in the game, start here. The
`docs/` folder next to this one explains *how* everything works internally;
this folder only explains *what to click and what to type*.

## 1. What this actually is

`tesmioloader` is a small program that starts the game for you and, while it
is starting, adds extra features to it — new resources, new deposits, new
buildings, and so on. **It does not change any file the game came with.**
Everything it adds either lives in memory while the game runs, or is written
into `media_soviet\workshop_wip\`, the same folder the game already uses for
your own unpublished Workshop items. Steam's "verify integrity of game files"
will not complain and will not undo anything.

Each feature is a separate **plugin** — a `.dll` file in the `plugins`
folder, with one settings file (`.ini`) sitting right next to it. You can
turn each plugin on or off independently, and you can edit its `.ini` to
change what it does, without touching anything else.

## 2. The two folders you need to know

```
tesmioloader\build\tesmiolauncher.exe   <- you run this, not SOVIET64.exe
tesmioloader\build\tesmioloader.ini     <- master switch, per-plugin on/off
tesmioloader\build\plugins\*.ini        <- one settings file per feature
tesmioloader\build\tesmioloader.log     <- what happened last time you played
```

Everything you will ever need to open is inside `tesmioloader\build\`. You
never need to touch anything under `tesmioloader\src\`, `tesmioloader\tools\`
or `tesmioloader\ghidra\` — those are for the people building the loader
itself.

## 3. Starting the game

1. Make sure Steam is running (the launcher starts the real game underneath,
   it does not replace Steam).
2. Run `tesmioloader\build\tesmiolauncher.exe`.
3. A window opens showing the game path it found, and a checkbox for every
   plugin in the `plugins` folder. If the path is wrong, click **Browse** and
   point it at the folder that has `SOVIET64.exe` in it.
4. Tick the plugins you want active this session, untick the ones you don't.
5. Click the button that starts the game.

The launcher remembers your choice: it writes a `[plugins]` section into
`tesmioloader.ini` with one line per plugin, `name=1` for on and `name=0` for
off. You can also edit that section by hand in a text editor if you prefer —
see step 5 below.

**If the game won't start at all**, the fastest thing to try is unticking
every plugin (or setting `plugins=0` at the top of `tesmioloader.ini`) and
starting again. If that works, one plugin is the problem — re-enable them one
at a time to find out which, and check `tesmioloader.log` for the last few
lines before it stopped.

## 4. Changing a setting

Every plugin's behaviour is controlled by its `.ini` file. To change
anything described in the other guides in this folder:

1. **Close the game first.** The loader reads each `.ini` when the game
   starts; it does not notice a file changing while the game is running.
2. Open the file — e.g. `tesmioloader\build\plugins\resources.ini` — in
   Notepad or any plain text editor.
3. Find the line you want (they're all commented, so read the text above the
   line you're changing if you're not sure what it does).
4. Change the value after the `=` sign. Leave everything else alone.
5. Save the file **as UTF-8 without a BOM**. Windows' own Notepad does this
   correctly if you just pick "UTF-8" (not "UTF-8 with BOM") in the save
   dialog's encoding dropdown. **Do not use PowerShell's `-Encoding UTF8` to
   write these files** — it silently adds a BOM, which breaks the file's
   `[section]` headers and every setting in it quietly falls back to its
   default. This has bitten people before.
6. Start the game again through `tesmiolauncher.exe`.

A line starting with `;` is a comment — the game ignores it. To try one of
the ready-made examples in a file (there are several, e.g. a nickel deposit
or a sand deposit), delete the `;` at the start of each of its lines.

## 5. Turning a whole plugin on or off

Two equivalent ways:

- **From the launcher window** — tick or untick its checkbox before
  starting the game. This is the easiest way and does not require opening
  any file.
- **By hand** — open `tesmioloader\build\tesmioloader.ini`, find the
  `[plugins]` section at the bottom, and change the plugin's line to `=0`
  (off) or `=1` (on). Save it the same UTF-8-without-BOM way as above.

Turning a plugin off does not delete anything it already put in your save —
a building it added stays built, a resource already sitting in a warehouse
stays there. It only stops the plugin from doing anything *new*. Deleting the
plugin's `.dll` from the `plugins` folder is the permanent version of "off".

## 6. Reading the log

`tesmioloader\build\tesmioloader.log` is a plain text file, overwritten every
time you start the game. It is the only way to see whether something
actually worked, rather than just "the game didn't crash". Every plugin
guide in this folder tells you which words to look for.

Open it with Notepad. If it's long, use Ctrl+F and search for the plugin's
name, or for a specific word the guide told you to look for (e.g. `deplete`,
`battery`, `price`, `demand`).

## 7. Before you experiment: back up your save

Several plugins change things that live **inside your save file** — a new
resource, a new citizen need, a mine running down, a building added to the
map. These are not always safe to switch on and off again on an existing
city. Each guide in this folder says explicitly whether the feature it
covers is safe to toggle freely or not; when in doubt:

1. Go to your save folder (in-game: Load Game screen shows you the save
   name; the files live under your Documents / game user folder in
   `save\<number> - <name>\`).
2. Copy that whole folder somewhere else before you try anything new.
3. If a change goes wrong, close the game, delete the modified save folder,
   and put your copy back.

This single habit is the difference between "that didn't work, let me
adjust a number" and "I lost my city".

## 8. What's in this folder

| Guide | What it lets you do |
|---|---|
| [01-resources](01-resources.md) | Add a brand-new tradeable good (like a new ore or product) |
| [02-deposits](02-deposits.md) | Add a new kind of mineable deposit, paintable on the map |
| [03-depletion](03-depletion.md) | Make mines actually run out of ore over time |
| [04-accumulator](04-accumulator.md) | Build a battery that stores spare electricity |
| [05-needs](05-needs.md) | Give citizens a new thing to shop for |
| [06-walking](06-walking.md) | Change how far citizens will walk or drive |
| [07-buildings](07-buildings.md) | Add a whole new building by cloning an existing one |

Read a guide's own warnings before turning its plugin on — a couple of them
(resources, needs, depletion) touch the save file in ways that are not
always reversible on a city you already care about.
