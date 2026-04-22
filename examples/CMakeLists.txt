foreach(example basic routing async streaming tls workers file)
    add_executable(example_${example} ${example}.c)
    target_compile_options(example_${example} PRIVATE ${LIBHUV_WARNINGS})
    target_link_libraries(example_${example} PRIVATE huv)
endforeach()

# example_tls needs the generated cert to be present before it can run.
add_dependencies(example_tls huv_tls_cert)
