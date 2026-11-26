set(CMAKE_AUTOMOC ON)

add_executable(p2p-gui
    main.cpp
    mainwindow.cpp
    daemonclient.cpp
)
target_link_libraries(p2p-gui PRIVATE Qt6::Widgets Qt6::Network)

# Кладемо p2p-gui поруч із p2pd, щоб GUI міг сам знайти й запустити демон
set_target_properties(p2p-gui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
