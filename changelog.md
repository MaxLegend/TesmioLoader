## Changelog

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