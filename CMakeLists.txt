cmake_minimum_required(VERSION 3.16)
project(agent_runtime)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)



add_executable(agent_runtime
    src/main.cpp
    src/http_server.cpp
    src/backend.cpp
    src/mock_backend.cpp
    src/openai_backend.cpp
    src/runtime.cpp
    src/scheduler.cpp
    src/session_manager.cpp
    src/metrics.cpp
)
target_include_directories(agent_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(agent_runtime pthread)