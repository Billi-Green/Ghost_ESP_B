#!/bin/bash
# Script to build the HTML header file using Docker

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$SCRIPT_DIR"

# Build the Docker image if it doesn't exist or if forced
if [ "$1" = "--rebuild" ] || ! docker image inspect ghost-html-converter:py36 >/dev/null 2>&1; then
    echo "Building Docker image with Python 3.6..."
    docker build -t ghost-html-converter:py36 .
fi

# Run the container with proper volume mounts
# Mount the project root so the script can write directly to include/managers/
echo "Running html_to_header.py in Docker container..."
docker run --rm \
    -v "$PROJECT_ROOT:/workspace" \
    -w /workspace/scripts/site \
    ghost-html-converter:py36 \
    python3 html_to_header.py

# Verify the file was created
if [ -f "$PROJECT_ROOT/include/managers/ghost_esp_site_gz.h" ]; then
    echo "✓ Successfully generated header file at $PROJECT_ROOT/include/managers/ghost_esp_site_gz.h"
    ls -lh "$PROJECT_ROOT/include/managers/ghost_esp_site_gz.h"
else
    echo "✗ Error: Header file was not generated!"
    exit 1
fi

