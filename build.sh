#!/bin/bash
# ZepraBrowser Build Script - Uses Ninja for faster, lighter builds
# Usage: ./build.sh [clean|release|debug]

set -e

BUILD_TYPE="${1:-debug}"
BUILD_DIR="build_ninja"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ZepraBrowser Build (Ninja) ===${NC}"

# Check for Ninja
if ! command -v ninja &> /dev/null; then
    echo -e "${YELLOW}Warning: Ninja not found. Installing...${NC}"
    sudo apt-get install -y ninja-build 2>/dev/null || {
        echo -e "${RED}Failed to install Ninja. Install manually: sudo apt install ninja-build${NC}"
        exit 1
    }
fi

# Handle build type
case "$BUILD_TYPE" in
    clean)
        echo -e "${YELLOW}Cleaning build directory...${NC}"
        rm -rf "$BUILD_DIR"
        echo -e "${GREEN}Clean complete.${NC}"
        exit 0
        ;;
    release)
        CMAKE_BUILD_TYPE="Release"
        ;;
    debug|*)
        CMAKE_BUILD_TYPE="Debug"
        ;;
esac

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with Ninja generator
echo -e "${GREEN}Configuring with Ninja (${CMAKE_BUILD_TYPE})...${NC}"
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DUSE_NXRENDER=ON \
    ..

# Get number of cores and limit to half to prevent overload
NPROC=$(nproc)
JOBS=$((NPROC / 2))
if [ "$JOBS" -lt 1 ]; then
    JOBS=1
fi

echo -e "${GREEN}Building with $JOBS parallel jobs (of $NPROC cores)...${NC}"

# Build with Ninja (less memory usage than make)
ninja -j "$JOBS"

echo -e "${GREEN}=== Build Complete ===${NC}"
echo -e "Binary: ${BUILD_DIR}/bin/zepra_browser"