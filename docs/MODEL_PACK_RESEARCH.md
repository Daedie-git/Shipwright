# 3DS model packs with OoT Reloaded

## Conclusion

Djipi's 3DS Experience is a promising candidate, but compatibility with this local build is **not verified**. Do not install its entire pack over OoT Reloaded without inspecting the modules and resource overlaps first.

## Evidence obtained

The [Shipwright-PTBR project](https://github.com/GeraldoCruzeiro/Shipwright-PTBR) documents a combined installation of OoT Reloaded and selected 3DS models. Its [installer source](https://github.com/GeraldoCruzeiro/Shipwright-PTBR/blob/main/scripts/ptbr_graphics/install_3d_models.ps1) provides concrete integration evidence:

- Djipi's 3DS Experience: [GameBanana mod 477979](https://gamebanana.com/mods/477979).
- Playas' 3DS Adult/Young Link: [GameBanana mod 475743](https://gamebanana.com/mods/475743).
- It selects Djipi modules by filenames containing animal, inventory, temple, background, NPC, or enemy variants.
- It excludes main-texture, Link-texture, world/object, scene, and optional modules from that selection to avoid replacing the Reloaded base.
- It installs the separate Playas Link pack and names the additions with a `ZZZ_` prefix to load after the base textures.
- It describes installing paired model/textures, not stripping all textures out of model packs. Replacement geometry can require different texture coordinates and associated textures.

This proves another project attempts the combination. It does **not** establish compatibility with unmodified Shipwright 9.3.0: the integration project is a separate fork, its selection is based on filenames, and its code is not the authors' compatibility documentation.

## Blocked verification

GameBanana returned HTTP 403 for the mod pages, legacy metadata API, and current profile API from this environment. Therefore the following remain unknown:

- Current pack version, download size, actual module filenames, and minimum supported SoH version.
- Authors' current warnings and required dependencies.
- Exact resource overlap with the installed Reloaded HD v11.0.0 pack.
- Runtime behaviour on our build.

No model archives were downloaded or installed. OoT Reloaded remains unchanged.

## Next step

Obtain the current Djipi main archive and, optionally, the Playas Link archive from their author pages. Inspect the downloaded archives without executing their contents, enumerate `.otr`/`.o2r` resources, and compare overlaps with Reloaded. Start with a small character-model subset and its matching textures, then test startup, file selection, gameplay, equipment, and animation before enabling the wider selection.
