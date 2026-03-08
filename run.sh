#!/bin/bash
# Run hello_world (or any LibAnyar app) with snap GTK interference removed.
# Usage:  ./run.sh              (runs ./hello_world)
#         ./run.sh ./my_app     (runs a custom binary)

# Clear snap-imposed GTK/GDK variables that conflict with system GTK
unset GTK_PATH
unset GTK_EXE_PREFIX
unset GTK_IM_MODULE_FILE
unset GDK_BACKEND

# Point to system schemas (snap's schemas are incompatible)
export GSETTINGS_SCHEMA_DIR="/usr/share/glib-2.0/schemas"
export GIO_MODULE_DIR="/usr/lib/x86_64-linux-gnu/gio/modules"

# Ensure system library paths take priority
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"

BINARY="${1:-./hello_world}"
exec "$BINARY" "${@:2}"
