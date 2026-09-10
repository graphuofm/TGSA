#!/bin/bash
# Drop page cache and exec the given command. Requires sudo for drop_caches;
# falls back to a soft sync if sudo is unavailable.
set -e

if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sync
    sudo -n sysctl -q vm.drop_caches=3 || true
else
    sync
fi
sleep 0.2
exec "$@"
