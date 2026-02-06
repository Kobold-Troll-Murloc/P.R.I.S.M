cmake_minimum_required(VERSION 3.10)
project(PRISM)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define paths
set(OGRE_NEXT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ogre-next")
set(OGRE_NEXT_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ogre-next/build")
set(OGRE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ogre-next-deps/build/ogredeps")

# Include directories
include_directories(
    include
    "${OGRE_NEXT_SOURCE_DIR}/OgreMain/include"
    "${OGRE_NEXT_BUILD_DIR}/include"
    "${OGRE_NEXT_SOURCE_DIR}/Components/Hlms/Common/include"
    "${OGRE_NEXT_SOURCE_DIR}/Components/Hlms/Pbs/include"
    "${OGRE_NEXT_SOURCE_DIR}/Components/Hlms/Unlit/include"
    "${OGRE_NEXT_SOURCE_DIR}/Components/Overlay/include"
    "${OGRE_NEXT_SOURCE_DIR}/Samples/2.0/Common/include"
    "${OGRE_DEPS_DIR}/include"
    "${OGRE_DEPS_DIR}/include/SDL2"
)

# Source files
file(GLOB_RECURSE SOURCE_FILES "src/*.cpp" "src/*.c")
file(GLOB_RECURSE HEADER_FILES "include/*.h" "include/*.hpp")

# Library directories
link_directories(
    "${OGRE_NEXT_BUILD_DIR}/lib/Release"
    "${OGRE_DEPS_DIR}/lib/Release"
)

add_executable(PRISM ${SOURCE_FILES} ${HEADER_FILES})

# Link libraries (Simplified for example, might need more)
target_link_libraries(PRISM
    OgreMain
    OgreHlmsPbs
    OgreHlmsUnlit
    OgreOverlay
    SDL2
    SDL2main
)

# Copy configuration files for running
add_custom_command(TARGET PRISM POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OGRE_NEXT_BUILD_DIR}/bin/Release/resources2.cfg"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/resources2.cfg"
)
