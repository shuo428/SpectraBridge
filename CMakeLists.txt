cmake_minimum_required(VERSION 3.20)
project(SpectraBridge)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(JNI REQUIRED)

add_library(SpectraBridgeJni SHARED
    src/bridge/JniBridge.cpp
    src/bridge/JniExports.cpp
    src/image/ImageConverter.cpp
    src/network/TcpClient.cpp
    src/service/SpectraBridgeClient.cpp
    src/util/Crc32.cpp
)

target_include_directories(SpectraBridgeJni PRIVATE
    include
    generated/jni
    ${JNI_INCLUDE_DIRS}
)

set_target_properties(SpectraBridgeJni PROPERTIES
    OUTPUT_NAME "SpectraBridgeJni"
)

add_executable(MockFpgaServer
    tools/mock-fpga/MockFpgaServer.cpp
)

if (WIN32)
    target_link_libraries(SpectraBridgeJni PRIVATE ws2_32)
    target_link_libraries(MockFpgaServer PRIVATE ws2_32)
endif()
