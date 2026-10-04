cmake_minimum_required(VERSION 3.20)
project(far-sight VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# llama.cpp: only the core library + libmtmd, statically linked into one binary.
# GPU backends are llama.cpp options, e.g. -DGGML_CUDA=ON or -DGGML_VULKAN=ON.
set(FARSIGHT_LLAMA_TAG "b11391" CACHE STRING "llama.cpp git tag to build against")

set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP       OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_MTMD      ON  CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL         OFF CACHE BOOL "" FORCE)
set(MTMD_VIDEO            OFF CACHE BOOL "" FORCE)

include(FetchContent)
# Set FETCHCONTENT_SOURCE_DIR_LLAMA_CPP=/path/to/llama.cpp to build against a local checkout.
FetchContent_Declare(llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${FARSIGHT_LLAMA_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(llama_cpp)

find_package(Threads REQUIRED)

add_executable(far-sight
    src/main.cpp
    src/options.cpp
    src/camera.cpp
    src/image.cpp
    src/vlm.cpp
    src/events.cpp
)
target_include_directories(far-sight PRIVATE src third_party)
target_link_libraries(far-sight PRIVATE llama mtmd Threads::Threads)
target_compile_options(far-sight PRIVATE -Wall -Wextra)
set_source_files_properties(src/image.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers;-Wno-unused-function")

include(GNUInstallDirs)
# Install just the binary: cmake --install build --component far-sight --prefix ~/.local
install(TARGETS far-sight RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT far-sight)
