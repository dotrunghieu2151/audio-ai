find_package(cubeb CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE cubeb::cubeb)

target_sources(
    ${PROJECT_NAME}
    PRIVATE
      cubeb_backend.cpp
      cubeb_stream.cpp
)
