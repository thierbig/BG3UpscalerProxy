# BG3UpscalerProxy

[![GitHub Release](https://img.shields.io/github/v/release/thierbig/bg3upscalerproxy)](https://github.com/thierbig/bg3upscalerproxy/releases)

A proxy DLL that enables compatibility between Baldur's Gate 3 upscaler (DLSS) and Script Extender. Consequently, other mods requiring SE such as Mod Configuration Menu (MCM) will become usable.

## Overview

This proxy DLL solves compatibility issues between upscaler injections and the BG3 Script Extender by acting as an intermediary loader that ensures proper initialization order. It allows you to enjoy enhanced graphics with DLSS upscaling while still being able to use Script Extender mods.

## Screenshots

### DLSS Upscaler and Script Extender Working Together (check edge of screen for DLSS versions and FG versions)
![DLSS Upscaler](https://i.imgur.com/YnhJZUj.png)

### Mod Configuration Menu (requires Script Extender) Working with Upscaler
![MCM Working](https://i.imgur.com/OHn8rZs.png)

## Features

- Enables simultaneous use of upscaler, Script Extender, and other mods requiring SE such as Mod Configuration Menu
- Properly sequences the loading of components to avoid crashes and conflicts
- Detailed logging for troubleshooting in the `UpscalerProxy.log` file

## Prerequisites

- You must purchase the original BG3 upscaler injection from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
- You need the [BG3 Script Extender](https://github.com/Norbyte/bg3se) installed
- This only works with Baldur's Gate 3 on PC

## Installation

1. Purchase and activate the original upscaler from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
2. **Rename** the Script Extender's original `DWrite.dll` to `ScriptExtender.dll` in your Baldur's Gate 3 bin folder
3. **Copy** the `DWrite.dll` from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) to your Baldur's Gate 3 bin folder
4. **(Optional but Recommended)** Replace PureDark's `mods` folder with the one from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) for access to DLSS4 and MFG

## Why Use This Modified "mods" Folder Instead of PureDark's official link?

This version offers these improvements:
- DLSS 4 instead of DLSS 3 (better performance and visual quality)
- Access to Multi-Frame Generation (MFG) in addition to Frame Generation (FG)

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

This project is provided as-is with no warranty. Use at your own risk.
