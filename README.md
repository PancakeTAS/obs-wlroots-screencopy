# obs-wlroots-screencopy
OBS Studio plugin for efficient screen capture on wlroots-based wayland compositors.

While most compositors have a screencapture implementation through pipewire, it quite frankly just sucks. For some reason, it slows down my games (from 60 fps limited to 30 fps.. like what the hell) and the annoying popup you get on Hyprland is also just annoying.
Fortunately, the desktop portal and pipewire simply use a wlroots protocol called ZwlrScreencopyManagerV1, which is what this plugin uses to capture the screen.

## Requirements
Your compositor needs to support these experimental protocols:
- [Linux DMA-BUF](https://wayland.app/protocols/linux-dmabuf-v1)
- [wlr screencopy](https://wayland.app/protocols/wlr-screencopy-unstable-v1)

(see compositor support at the bottom of the page)

## Installation
1. Clone this repository
2. Run `make PROD=1`
3. Copy the resulting `obs-wlroots-screencopy.so` to your OBS Studio plugin directory (usually `~/.config/obs-studio/plugins/obs-wlroots-screencopy/bin/64bit/`)
4. Restart OBS Studio

## Usage
1. Add a new source to your scene
2. Select `Screencopy Source` from the list of available sources
3. Configure the source
