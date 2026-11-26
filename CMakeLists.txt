cmake_minimum_required(VERSION 3.20)
project(p2p-messenger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

find_package(SQLite3 REQUIRED)

add_executable(p2pd
    src/main.cpp
    src/protocol/packet.cpp
    src/storage/storage.cpp
    src/ipc/ipc.cpp
)
target_include_directories(p2pd PRIVATE src)
target_link_libraries(p2pd PRIVATE SQLite::SQLite3)

# Графічний клієнт (потребує Qt 6); якщо Qt немає, збирається лише демон
find_package(Qt6 COMPONENTS Widgets Network)
if(Qt6_FOUND)
    add_subdirectory(gui)
else()
    message(WARNING "Qt6 не знайдено: GUI-клієнт не буде зібрано")
endif()
