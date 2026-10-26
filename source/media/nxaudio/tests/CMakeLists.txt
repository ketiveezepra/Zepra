# NXAudio tests

add_executable(nxaudio_output_test audio_output_test.c)
target_include_directories(nxaudio_output_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(nxaudio_output_test PRIVATE nxaudio_static)

# The null device plays in real time without sound hardware (CI).
# Run the binary directly to hear it on the real device.
add_test(NAME NxAudioOutputTest
         COMMAND nxaudio_output_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(NxAudioOutputTest PROPERTIES
                     ENVIRONMENT "NXAUDIO_BACKEND=null"
                     TIMEOUT 60)
