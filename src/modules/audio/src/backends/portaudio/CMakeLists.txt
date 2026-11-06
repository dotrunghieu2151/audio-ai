find_package(portaudio CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE portaudio_static)

target_sources(
    ${PROJECT_NAME}
    PRIVATE
      portaudio_backend.cpp
      portaudio_stream.cpp
)