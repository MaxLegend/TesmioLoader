## Changelog

---

*Update! - v. b0.3.7*
The game's regular branch updated to v1.1.1.13. Measured against a byte copy of the old build: `.text` grew by only 0x1A0 but reflowed in the usual steps (0, -0x80, -0x70, +0x1A0 - most of the update's changes landed *before* the loader's sites, moving them down), the `.rdata` constant pool moved forward for once (+0x8/+0x10 in two clusters), `.data` did not move a single byte, and C3DDLL64.dll changed only 34 bytes of timestamps - the engine is untouched. Every hard-coded address across all nine active plugins and the four address keys in `resources.ini` has been re-derived against the new build and verified: 34 of the 36 install-time byte checks carried over unchanged, two (`kMapOpenOrig`/`kMapCloseOrig`) carry a displacement that moved with their site. v1.1.1.9 is no longer supported. A full record of what the update moved - including the day-length note from b0.3.6's port, which turned out to be a misread of a neighbouring pool constant - is in docs/17-port-v1.1.1.9.md.

---

*Update! - v. b0.3.6*
The game's regular branch updated to v1.1.1.9 - the update notes said content only, but the executable itself had in fact reflowed throughout: `.text` grew in steps of up to nearly 500 bytes depending how far into it a given address sat, and the `.rdata` constant pool shifted in two separate clusters. Every hard-coded address across all nine active plugins has been re-derived and re-verified byte for byte against the new build; v1.1.1.7 is no longer supported, and none of its addresses remain in the source.
The first launch on the new build crashed: `resources` was installing an import hook before its main hook, and when the main one refused (address moved), the loader freed the plugin with the import hook still pointing into it. Fixed - the import hook now installs only after the main one is confirmed.
Two plugins, `aging` and `buildings`, are parked for now - `build.bat` skips both folders, so neither ships a DLL until they get a proper look. Nothing about either is deleted.
Some stale comments describing an old, pre-plugin-split version of the loader (a resource-name hook that has lived in `plugins\resources` for a long time) were cleared out of `tesmioloader.cpp` - no behaviour changed, just what the file claims about itself.

---

*Update! - v. b0.3.5*
Game version control. The launcher now reads the version out of SOVIET64.exe before it starts anything, says in its window which version is required and which one is installed, and refuses to launch a game that is not v1.1.1.7 - every address the loader patches belongs to that build, and injecting into another one is what makes the game die on startup with nothing to explain it. `version_check = 0` in tesmioloader.ini, or `--ignore-version`, turns the refusal back into a warning.
The plugin list is now two columns with a scroll bar, so a long list no longer pushes the Launch button off the bottom of the screen. The launcher window carries the tesmioloader logo in its title bar.
Signature marks are quieter: a plugin built by me shows `[tesmio]`, anything unsigned shows nothing at all instead of being labelled "not from Tesmio", and only a signature that exists and does not match its file is called out.
Plugins built against an older API version now keep working where that is possible: the loader accepts a range of API versions rather than one exact number, and a change that does not have to break an old plugin is not allowed to.

---

*Update! - v. b0.3.4*
A signature of my authorship has been added - modified plugins or plugins without a signature will be marked as not signed. This does not affect anything - any other plugins will load. It's just my digital signature that the plugin was built by me and not modified in any way.
Fixed bugs and crashes: issues #8, issues #9, issues #10

---

*Update! - v. b0.3.3*
A cross-reference bug that was used during development and accidentally made it into the release has been fixed. This was the reason why many users often failed to search for icons and resources, even though they were physically located where they should have been.

---

*Update! - v. b0.3.2*
Added version control - the game will no longer crash if the plugin version does not match the launcher version - it will notify you of this during the initialization phase.

---

*Update! - v. b0.3.1*
Minor fixes. Added a launcher logo. Fixed a save error causing crashes due to deposits.dll.

---

*Update! - v. b0.3*
The launcher now has a window. It shows where the game file was found, with a Browse button if it was found incorrectly, and a checkbox for each plugin. Uncheck it, and the plugin will remain on disk but will not load, so you can disable this feature without deleting anything. Your choice is remembered.
Critical bugs in the resource and deposit plugins have also been fixed, and tooltips have been added to custom buttons.

---

*Update! - v. a0.2.1*
Minor fixes and refactoring

---

*Update! - v. a0.2*
The loader has been updated to version a0.2—the code has been completely refactored and the architecture has been changed to ensure greater unification across separate plugins. If you have already installed the loader, please completely delete this folder and install the updated version.
The loader is now separated from the code, allowing you to install functions separately—the resource plugin, the deposit plugin, or the depletion plugin.