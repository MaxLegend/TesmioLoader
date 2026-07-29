## A new era of modding begins here

**Hello, comrade!**

I'm pleased to present my custom loader for the game WRSR. It allows you to truly modify the game through code injection — adding new resources, including new deposits on the map, as well as full-fledged interaction with those new resources. I'm more than confident that this loader is the beginning of a future of large-scale and genuine modding. In the long run, it opens up possibilities across a wide range of game improvements that players have long dreamed of:
- Modding infrastructure, roads, footpaths, fences
- Changing economic formulas, modifying pathfinding formulas and algorithms
- Modifying travel range, resident working hours
- Modifying road placement algorithms
- And finally, even adding new mechanics and much, much more

The mod will require manual installation: copy the folder to the game root directory, next to media_soviet. After that, open the folder and run the loader — tesmiolauncher.exe. Steam must also be running — the launcher simply runs the same SOVIET64.exe, injecting my tesmioloader.dll library, which contains the patching code. 
The modification works on the principle of hooks — the executable binary is patched during the game's loading process. When launching the game normally through Steam, the unmodified version runs. 

*Update! - v. b0.3*
The launcher now has a window. It shows where it found the game, with a Browse button if it got it wrong, and a checkbox for every plugin — untick one and it stays on disk but is not loaded, so you can turn a feature off without deleting anything. Your choice is remembered.

You no longer have to put the folder in exactly the right place. The launcher looks for SOVIET64.exe next to itself, then in the folders above it and one level inside each of those, and finally through Steam's own library list — so it finds the game even if the folder ended up on your desktop. Putting it in the game root is still the tidiest option.

*Update! - v. a0.2*
The loader has been updated to version a0.2 – a complete code refactoring and architecture changes have been implemented to ensure greater unification through separate plugins. If you've already installed the loader, please completely delete this folder and install the updated one.
The loader is now separated from the code, allowing you to install features separately – the resource plugin, the deposit plugin, or the depletion plugin.

This was no easy task, so please support me on Boosty — https://boosty.to/tesmio/donate

Please report any bugs in the issues section on Github. Important — WRSR itself often crashes on its own. Please run it two or three times to make sure the crash is actually caused by my loader. 

The launcher is distributed under the GNU GPL v3 license. The full license text is available here → https://choosealicense.com/licenses/gpl-3.0/  

Additionally, I am also developing a new game called Socialist Union. You can find more details in the development diaries on my YouTube channel — https://www.youtube.com/@tesmio