# BG3UpscalerProxy

[![GitHub Release](https://img.shields.io/github/v/release/thierbig/bg3upscalerproxy)](https://github.com/thierbig/bg3upscalerproxy/releases)

## Latest Version DLSS FG ONLY

**[Download .dll from link and follow readme here below](https://github.com/thierbig/bg3upscalerproxy/releases/latest)**

## Overview

A modified Script Extender that enables compatibility with DLSS FG from PureDark.

## Screenshots

### Mod Configuration Menu (requires Script Extender) Working with Upscaler(top-left corner)
<img width="2559" height="1439" alt="image" src="https://github.com/user-attachments/assets/ea7529c4-2a71-478a-bf79-af42d57c01fe" />

## Features

- Enables simultaneous use of PureDark's FG upscaler, Bg3 Script Extender and compatible mods
- Enables MCM menu with DLSS FG
- Only is compatible with Vulkan

## Prerequisites

- You must purchase the original BG3 upscaler injection from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
- You need the [BG3 Script Extender](https://github.com/Norbyte/bg3se) installed and used once
- This only works with Baldur's Gate 3 on PC
- Use Vulkan

## Installation

1. Purchase and activate DLSS FG from [PureDark's Patreon](https://www.patreon.com/posts/bg3-upscaler-fg-89557958)
3. **Rename** PureDark's DLSS FG `version.dll` to `upscaler.dll` in your Baldur's Gate 3 bin folde
4. **Copy** the `BG3ScriptExtender.dll` from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases) to folder containing the latest version folder of Script Extender (i.e `C:\Users\User\AppData\Local\BG3ScriptExtender\ScriptExtender\27....` (highest number is good number)
5. **Replace** PureDark's `Streamline` folder with the one from [the releases page](https://github.com/thierbig/bg3upscalerproxy/releases/latest)


**Important:** 
- Only works for Nvidia GPUs series 4000/5000 because of DLSS4 MFG support
- Only Vulkan

**Note:** You still need to purchase PureDark's plugin for authentication.

## Known Issues

- **MCM Menu Flickering**: The Mod Configuration Menu will flicker and will be harder than usual to use
- **You might have to switch between Fullscreen and Fullscreen borderless for the MCM menu to appear**

## OLD VERSION COMPATIBLE WITH DX11 AND/OR FSR (NO MCM Menu) 

FOLLOW THE README ON LINK: [https://github.com/thierbig/BG3UpscalerProxy/releases/tag/3.0](https://github.com/thierbig/BG3UpscalerProxy/releases/tag/3.0)

## Credits

- Original upscaler injection by [PureDark](https://www.patreon.com/pureDark)
- Script Extender by [Norbyte](https://github.com/Norbyte/bg3se)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
