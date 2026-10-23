#!/usr/bin/env bash
# ==============================================================================
# TeamTalk Native Media Daemon - Launcher Script
# Author: Ivan (Ivankolp) | Space Project (@Space_project_fl) | @flastik_blog
# ==============================================================================

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$DIR"

echo "=========================================================="
echo "   TeamTalk Native Media Bot (C++ Edition)               "
echo "   Author: Ivan (Ivankolp) | Space Project                "
echo "=========================================================="

# 1. Check config
if [ ! -f "config.json" ]; then
    if [ -f "config.example.json" ]; then
        echo "[!] config.json not found. Creating from config.example.json..."
        cp config.example.json config.json
        echo "[!] Please edit config.json with your TeamTalk credentials and rerun!"
        exit 1
    else
        echo "[x] Error: Neither config.json nor config.example.json found!"
        exit 1
    fi
fi

# 2. Check dependencies
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[!] Warning: 'ffmpeg' is not found in PATH! Installing ffmpeg is required for audio decoding."
fi

if ! command -v yt-dlp >/dev/null 2>&1; then
    echo "[i] Note: 'yt-dlp' is not found. Bot will use internal HTTP bridge if available."
fi

# 3. Check binary
if [ ! -f "tt-media-bot" ]; then
    echo "[*] Binary not found. Building project..."
    make -j$(nproc)
    strip tt-media-bot
fi

# 4. Set library path and run
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
exec "$DIR/tt-media-bot" "$@"
