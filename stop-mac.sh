#!/bin/sh
pkill -x web_server 2>/dev/null && echo "Server gestoppt." || echo "Server lief nicht."
pkill -f "dns-sd.*shikgoo" 2>/dev/null && echo "mDNS-Eintrag entfernt." || true
