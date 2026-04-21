cmake_minimum_required(VERSION 3.20)
project(SpectraBridge)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SpectraBridge
    main.cpp
    src/bridge/JniBridgePlaceholder.cpp
    src/image/ImageConverter.cpp
    src/network/TcpClient.cpp
    src/service/SpectraBridgeClient.cpp
    src/util/Crc32.cpp
)

target_include_directories(SpectraBridge PRIVATE include)

if (WIN32)
    target_link_libraries(SpectraBridge PRIVATE ws2_32)
endif()
