cmake_minimum_required(VERSION 3.16)

project(pcie_memory_master VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(yalantinglibs CONFIG REQUIRED)

add_library(pcie_master_core
    src/master_service.cpp
)
target_include_directories(pcie_master_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(pcie_master_core PRIVATE /W4)
else()
    target_compile_options(pcie_master_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(pcie_master src/main.cpp)
target_link_libraries(pcie_master PRIVATE
    pcie_master_core
    yalantinglibs::yalantinglibs
    Threads::Threads
)

add_executable(pcie_client
    src/client.cpp
    src/line_editor.cpp
)
target_link_libraries(pcie_client PRIVATE
    pcie_master_core
    yalantinglibs::yalantinglibs
    Threads::Threads
)

include(CTest)
if(BUILD_TESTING)
    add_executable(pcie_master_test tests/master_service_test.cpp)
    target_link_libraries(pcie_master_test PRIVATE pcie_master_core Threads::Threads)
    add_test(NAME pcie_master_test COMMAND pcie_master_test)

    add_executable(pcie_rpc_test tests/rpc_test.cpp)
    target_link_libraries(pcie_rpc_test PRIVATE
        pcie_master_core
        yalantinglibs::yalantinglibs
        Threads::Threads
    )
    add_test(NAME pcie_rpc_test COMMAND pcie_rpc_test)
endif()
