#!/usr/bin/env bash
# ==============================================================================
# TeamTalk Native Media Daemon - Launcher Script
# Author: Flastik | @flastik_blog
# ==============================================================================

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$DIR"

CFG="${1:-config.json}"

# 1. Check config
if [ ! -f "$CFG" ]; then
    if [ -f "config.example.json" ]; then
        echo "[!] Config '$CFG' not found. Creating from config.example.json..."
        mkdir -p "$(dirname "$CFG")"
        cp config.example.json "$CFG"
        echo "[!] Created template config at $CFG. Edit credentials and rerun!"
        exit 1
    else
        echo "[x] Error: Neither '$CFG' nor config.example.json found!"
        exit 1
    fi
fi

# 2. Check dependencies
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[!] Warning: ffmpeg is not found in PATH. ffmpeg is required for audio decoding."
fi

if ! command -v yt-dlp >/dev/null 2>&1; then
    echo "[i] Note: yt-dlp is not found. Bot will use internal HTTP bridge if available."
fi

# 3. Check binary
if [ ! -f "tt-media-bot" ]; then
    echo "[*] Binary not found. Building project..."
    make -j$(nproc)
    strip tt-media-bot
fi

# 4. Set library path and run
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
exec "$DIR/tt-media-bot" "$CFG"
