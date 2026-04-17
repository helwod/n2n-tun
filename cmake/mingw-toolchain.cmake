# MinGW Toolchain File for n2n Cross-Compilation
#
# Usage:
#   Linux/macOS host:
#     cmake -B build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake
#
#   Windows host with MSYS2/MinGW:
#     cmake -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW compiler paths (adjust if needed)
if(NOT DEFINED MINGW_PREFIX)
    if(WIN32)
        # Windows host - common MinGW installation paths
        if(DEFINED ENV{MINGW_PREFIX})
            set(MINGW_PREFIX $ENV{MINGW_PREFIX})
        elseif(DEFINED ENV{MSYSTEM_PREFIX})
            set(MINGW_PREFIX $ENV{MSYSTEM_PREFIX})
        elseif(EXISTS "C:/msys64/mingw64")
            set(MINGW_PREFIX C:/msys64/mingw64)
        elseif(EXISTS "C:/msys64/clang64")
            set(MINGW_PREFIX C:/msys64/clang64)
        elseif(EXISTS "C:/MinGW")
            set(MINGW_PREFIX C:/MinGW)
        else()
            message(FATAL_ERROR "MinGW not found. Please set MINGW_PREFIX or install MSYS2/MinGW-w64.")
        endif()
    else()
        # Unix-like host
        set(MINGW_PREFIX /usr/x86_64-w64-mingw32)
        if(NOT EXISTS "${MINGW_PREFIX}/bin")
            set(MINGW_PREFIX /usr/bin/x86_64-w64-mingw32)
        endif()
    endif()
endif()

message(STATUS "Using MinGW prefix: ${MINGW_PREFIX}")

# Set compiler paths
set(CMAKE_C_COMPILER ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER ${MINGW_PREFIX}/bin/x86_64-w64-mingw32-windres)

# Adjust target environment
set(CMAKE_FIND_ROOT_PATH ${MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Skip compiler tests (avoids linking issues)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Install paths for Windows binaries
set(CMAKE_INSTALL_PREFIX "C:/Program Files/n2n" CACHE PATH "Install prefix")
