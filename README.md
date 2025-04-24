# BG3UpscalerProxy

[![GitHub Release](https://img.shields.io/github/v/release/thierbig/bg3upscalerproxy)](https://github.com/thierbig/bg3upscalerproxy/releases)

## Quick Download

**[Download Latest Release Here](https://github.com/thierbig/bg3upscalerproxy/releases/latest)**

A proxy DLL that enables compatibility between Baldur's Gate 3 upscalers (DLSS FG, FSR3) and Script Extender. Consequently, other mods requiring SE such as Mod Configuration Menu (MCM) will become usable.

## Overview

This proxy DLL solves compatibility issues between upscaler injections and the BG3 Script Extender by acting as an intermediary loader that ensures proper initialization order. It allows you to enjoy enhanced graphics with DLSS or FSR3 upscaling while still being able to use Script Extender mods.

## Screenshots

### Mod Configuration Menu (requires Script Extender) Working with Upscaler(top-left corner)
![MCM Working](https://i.imgur.com/yFvsLKO.png)

## Features

- Enables simultaneous use of upscaler, Script Extender, and other mods requiring SE such as Mod Configuration Menu
- Works with DLSS or FSR3 or dlssg-to-fsr3
- The proxy is compatible with both DX11 and Vulkan
- Script Extender auto updater works
- Detailed logging for troubleshooting in the `UpscalerProxy.log` file

## Prerequisites

- You must purchase the original BG3 upscaler injection from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
- You need the [BG3 Script Extender](https://github.com/Norbyte/bg3se) installed
- This only works with Baldur's Gate 3 on PC
- When using the modified "mods" folder with DLSS4 FG, you **must** use Vulkan rendering mode (not DX11)

## Installation

1. Purchase and activate an upscaler from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958). He offers DLSS FG or FSR3
2. **Rename** the Script Extender's original `DWrite.dll` to `ScriptExtender.dll` in your Baldur's Gate 3 bin folder
3. **Copy** the `DWrite.dll` from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) to your Baldur's Gate 3 bin folder
4. **(Optional but Recommended)** Replace PureDark's `mods` folder with the one from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) for access to DLSS4 and MFG and Preset K

## Why Use This Modified "mods" Folder Instead of PureDark's official "mods" folder?

This version offers these improvements:
- DLSS 4 instead of DLSS 3 (better performance and visual quality)
- Access to Multi-Frame Generation (MFG) in addition to Frame Generation (FG)
- Uses Preset K by default for best DLSS performance available (4/24/2025)

**Important:** 
- The modified mods files are designed for Nvidia GPUs series 4000/5000 because of DLSS4 MFG support
- These modified mods files can **ONLY** be used with Vulkan as DX11 does not support DLSS4
- The modified "mods" folder should only be used with Vulkan and original Frame Generation (FG)
- FSR3 is untested and should only work with DX11 and with the original PureDark's mods file
- This tool is also compatible with [dlssg-to-fsr3](https://www.nexusmods.com/site/mods/738?tab=posts&BH=0) if installed

**Note:** You still need to purchase PureDark's plugin for authentication.

## Known Issues

- **MCM Menu Not Appearing Correctly**: The Mod Configuration Menu may not pop up correctly when using this proxy. Workaround: Set your MCM settings before installing this mod, then reload your game with this mod installed.
  - For the option to change MCM settings in-game, we need PureDark to publish a new version with the option to disable his overlay because it's conflicting with MCM. Ask him on Discord.
- **Save File Images Not Rendering Correctly**: In-game save file images may not be properly rendered. This is only a visual issue and does not affect functionality.

## Troubleshooting

If you experience any issues:
1. Check the `UpscalerProxy.log` file in your `mods` folder for error messages
2. Ensure you've properly renamed the original Script Extender DLL to `ScriptExtender.dll`
3. Verify that your GPU supports the upscaler features you're trying to use

### For Persistent Crashing Issues

1. Try deleting the `BG3ScriptExtender` folder in `%AppData%/Local` so the Script Extender can rebuild itself
2. If deleting the folder doesn't work, try setting `Manifest-Release.json` to read-only
3. Please report back if these solutions work for you, as they could be automated in future updates

## Credits

- Original upscaler injection by [PureDark](https://www.patreon.com/pureDark)
- Script Extender by [Norbyte](https://github.com/Norbyte/bg3se)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
