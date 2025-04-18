# BG3UpscalerProxy

[![GitHub Release](https://img.shields.io/github/v/release/thierbig/bg3upscalerproxy)](https://github.com/thierbig/bg3upscalerproxy/releases)

## Quick Download

**[Download Latest Release Here](https://github.com/thierbig/bg3upscalerproxy/releases/latest)**

A proxy DLL that enables compatibility between Baldur's Gate 3 upscaler (DLSS) and Script Extender. Consequently, other mods requiring SE such as Mod Configuration Menu (MCM) will become usable.

## Overview

This proxy DLL solves compatibility issues between upscaler injections and the BG3 Script Extender by acting as an intermediary loader that ensures proper initialization order. It allows you to enjoy enhanced graphics with DLSS upscaling while still being able to use Script Extender mods.

## Screenshots

### Mod Configuration Menu (requires Script Extender) Working with Upscaler(top-left corner)
![MCM Working](https://i.imgur.com/yFvsLKO.png)

## Features

- Enables simultaneous use of upscaler, Script Extender, and other mods requiring SE such as Mod Configuration Menu
- Properly sequences the loading of components to avoid crashes and conflicts
- Detailed logging for troubleshooting in the `UpscalerProxy.log` file

## Prerequisites

- You must purchase the original BG3 upscaler injection from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
- You need the [BG3 Script Extender](https://github.com/Norbyte/bg3se) installed
- This only works with Baldur's Gate 3 on PC
- When using the modified mods files with DLSS4, you **must** use Vulkan rendering mode (not DX11)

## Installation

1. Purchase and activate the original upscaler from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
2. **Rename** the Script Extender's original `DWrite.dll` to `ScriptExtender.dll` in your Baldur's Gate 3 bin folder
3. **Copy** the `DWrite.dll` from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) to your Baldur's Gate 3 bin folder
4. **(Optional but Recommended)** Replace PureDark's `mods` folder with the one from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) for access to DLSS4 and MFG

## Why Use This Modified "mods" Folder Instead of PureDark's official link?

This version offers these improvements:
- DLSS 4 instead of DLSS 3 (better performance and visual quality)
- Access to Multi-Frame Generation (MFG) in addition to Frame Generation (FG)

**Important:** 
- The modified mods files can **ONLY** be used with Vulkan as DX11 does not support DLSS4
- This project has been tested exclusively with Vulkan

**Note:** You still need to purchase PureDark's plugin for authentication.

## Troubleshooting

If you experience any issues:
1. Check the `UpscalerProxy.log` file in your `mods` folder for error messages
2. Ensure you've properly renamed the original Script Extender DLL to `ScriptExtender.dll`
3. Verify that your GPU supports the upscaler features you're trying to use

## Credits

- Original upscaler injection by [PureDark](https://www.patreon.com/pureDark)
- Script Extender by [Norbyte](https://github.com/Norbyte/bg3se)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
