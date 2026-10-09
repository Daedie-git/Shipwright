# Existing high-resolution menu textures

## Recommendation

Use GhostlyDark's **OoT Reloaded** rather than continuing the local menu redraw experiment. It already covers the title screen, file-selection labels, buttons, and panel textures, as well as the rest of the game. Installation and rendering still need to be tested against this checkout; matching the documented minimum version is not a runtime compatibility test.

## Primary-source findings

- The current [OoT Reloaded repository](https://github.com/GhostlyDark/OoT-Reloaded) describes approximately 20,000 textures, a target resolution of 2160p, official-language support, and support for Ship of Harkinian (SoH).
- The author's [download and instructions page](https://evilgames.eu/texture-packs/oot-reloaded.htm) lists release **v11.0.0**, dated **2025-04-13**. Its changelog explicitly includes file-select buttons, game logos, menu text, and file-select fonts among past improvements.
- The former [SoH repository](https://github.com/GhostlyDark/OoT-Reloaded-SoH) is archived and directs further development to the main repository. Its installation documentation requires SoH **v9.0.1 or newer**. This checkout declares **9.3.0** in `CMakeLists.txt`.
- That documentation says to put the supplied `.o2r` files in `mods/` and enable alternate assets with **Tab**. The author-hosted download page gives the same instructions.
- The [current release](https://github.com/GhostlyDark/OoT-Reloaded/releases/tag/v11.0.0) includes a **SoH O2R HD** archive (`oot-reloaded-v11.0.0-soh-o2r-hd.7z`, 626,160,013 bytes) and a **SoH O2R 4K** archive split into two downloads totaling 4,015,368,911 bytes. Select SoH files, not GLideN64, Dolphin, or rt64 files.
- The author describes the HD variant as downscaled to a maximum of **8x the original texture dimensions**. It is still higher-resolution than the local 4x experiment.

## Exact asset coverage verified

The archived SoH source contains replacements with the same resource names used by this checkout:

- [Title logo](https://github.com/GhostlyDark/OoT-Reloaded-SoH/tree/master/OoT%20Reloaded%20(SoH)/objects/object_mag): `gTitleZeldaShieldLogoTex.png`.
- [File-selection textures](https://github.com/GhostlyDark/OoT-Reloaded-SoH/tree/master/OoT%20Reloaded%20(SoH)/textures/title_static): `gFileSelPleaseSelectAFileENGTex.png`, `gFileSelControlsENGTex.png`, `gFileSelFile1ButtonENGTex.png`, and `gFileSelWindow1Tex.png`.

These filenames were verified through the GitHub contents API, rather than inferred from screenshots. The release archive itself has not been downloaded or inspected.

## Local next steps

1. Try the complete SoH O2R HD release first, or build a menu-only mod from the author's PNG sources if changing the rest of the game is undesirable.
2. Move `build-cmake/soh/mods/menu-upscale-sample.o2r` outside `mods/` while testing, to avoid replacement conflicts.
3. Restart with `oot`, enable alternate assets with Tab, and compare title, file-selection, options, and name-entry screens.

The local expanded redraw generator is written but has not been run; the installed experimental mod still contains the original seven replacement textures.
