cmake_minimum_required(VERSION 3.20)
project(p2p-messenger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

find_package(SQLite3 REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)

# Модулі демона, що не залежать від main.cpp (їх же використовують тести)
add_library(p2pcore STATIC
    src/protocol/packet.cpp
    src/crypto/crypto.cpp
)
target_include_directories(p2pcore PUBLIC src ${SODIUM_INCLUDE_DIR})
target_link_libraries(p2pcore PUBLIC ${SODIUM_LIBRARY})

add_executable(p2pd
    src/main.cpp
    src/storage/storage.cpp
    src/ipc/ipc.cpp
)
target_link_libraries(p2pd PRIVATE p2pcore SQLite::SQLite3)

# Модульні тести: ./build/p2p-tests або ctest --test-dir build
enable_testing()
add_executable(p2p-tests tests/test_core.cpp)
target_link_libraries(p2p-tests PRIVATE p2pcore)
add_test(NAME core COMMAND p2p-tests)

# Графічний клієнт (потребує Qt 6); якщо Qt немає, збирається лише демон
find_package(Qt6 COMPONENTS Widgets Network)
if(Qt6_FOUND)
    add_subdirectory(gui)
else()
    message(WARNING "Qt6 не знайдено: GUI-клієнт не буде зібрано")
endif()
