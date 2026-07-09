#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Get the script's directory and root of the template
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Verify arguments
if [ -z "$1" ]; then
    echo "Error: Missing project name."
    echo "Usage: $0 <new_project_name>"
    exit 1
fi

NEW_PROJECT_NAME="$1"
TARGET_DIR="$(dirname "$TEMPLATE_DIR")/$NEW_PROJECT_NAME"

if [ -d "$TARGET_DIR" ]; then
    echo "Error: Target directory '$TARGET_DIR' already exists."
    exit 1
fi

echo "Cloning template to '$TARGET_DIR'..."

# Create target directory
mkdir -p "$TARGET_DIR"

# Copy files using rsync or cp, excluding build/ and other artifacts
if command -v rsync >/dev/null 2>&1; then
    rsync -a --exclude="build/" --exclude="sdkconfig" --exclude=".git/" "$TEMPLATE_DIR/" "$TARGET_DIR/"
else
    cp -r "$TEMPLATE_DIR/." "$TARGET_DIR/"
    # Clean up copied build or sdkconfig in target if any
    rm -rf "$TARGET_DIR/build"
    rm -f "$TARGET_DIR/sdkconfig"
fi

# Rename project in the new CMakeLists.txt
CMAKE_FILE="$TARGET_DIR/CMakeLists.txt"
if [ -f "$CMAKE_FILE" ]; then
    echo "Updating project name in $CMAKE_FILE to '$NEW_PROJECT_NAME'..."
    # Support macOS and Linux sed syntax safely
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' "s/project(esp32-universal-template)/project($NEW_PROJECT_NAME)/g" "$CMAKE_FILE"
    else
        sed -i "s/project(esp32-universal-template)/project($NEW_PROJECT_NAME)/g" "$CMAKE_FILE"
    fi
else
    echo "Warning: CMakeLists.txt not found in new project root."
fi

# Clean build artifacts if they were copied
rm -rf "$TARGET_DIR/build"
rm -f "$TARGET_DIR/sdkconfig"
rm -f "$TARGET_DIR/sdkconfig.old"

echo "Success! New project '$NEW_PROJECT_NAME' created at '$TARGET_DIR'."
