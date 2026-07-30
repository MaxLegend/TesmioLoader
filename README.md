[English](README.md) | [Русский](README_RU.md) | [简体中文](README_zh-CN.md)

[Changelog](changelog.md) | [Журнал обновлений](changelog_ru.md)

[Documentation](https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started) | [Документация](https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started/RU)

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

The default version includes a set of the loader itself and two plugins for testing copper production chain (optional) - this is the resource and deposits plugin (resources.dll and deposits.dll) and the corresponding configuration files next to them.

This was no easy task, so please support me on Boosty — https://boosty.to/tesmio/donate

The launcher is distributed under the GNU GPL v3 license. The full license text is available here → https://choosealicense.com/licenses/gpl-3.0/  

Additionally, I am also developing a new game called Socialist Union. You can find more details in the development diaries on my YouTube channel — https://www.youtube.com/@tesmio
