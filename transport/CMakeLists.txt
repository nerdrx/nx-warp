# nxvc_transport - codec-agnostic transport for NX Warp.
# Normative wire format: docs/TRANSPORT.md.  No sockets, no key exchange.
add_library(nxvc_transport STATIC
    src/wire.cpp
    src/aead.cpp
    src/fec.cpp
    src/packetizer.cpp
    src/multipath.cpp
    src/shadow.cpp
    src/sender.cpp
    src/receiver.cpp
)
target_include_directories(nxvc_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(nxvc_transport PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(nxvc_transport PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Optional real AEAD backends.  The library builds and the tests pass without
# either; the simulator and the tests use the built-in NullAead by default.
option(NXVC_TRANSPORT_OPENSSL "Use OpenSSL for AES-256-GCM / ChaCha20-Poly1305" ON)
option(NXVC_TRANSPORT_SODIUM "Use libsodium when OpenSSL is absent" ON)

set(_nxt_crypto "none")
if(NXVC_TRANSPORT_OPENSSL)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        target_compile_definitions(nxvc_transport PRIVATE NXT_HAVE_OPENSSL)
        target_link_libraries(nxvc_transport PUBLIC OpenSSL::Crypto)
        set(_nxt_crypto "openssl")
    endif()
endif()
if(_nxt_crypto STREQUAL "none" AND NXVC_TRANSPORT_SODIUM)
    find_path(SODIUM_INCLUDE_DIR sodium.h)
    find_library(SODIUM_LIBRARY NAMES sodium libsodium)
    if(SODIUM_INCLUDE_DIR AND SODIUM_LIBRARY)
        target_compile_definitions(nxvc_transport PRIVATE NXT_HAVE_SODIUM)
        target_include_directories(nxvc_transport PRIVATE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(nxvc_transport PUBLIC ${SODIUM_LIBRARY})
        set(_nxt_crypto "sodium")
    endif()
endif()
message(STATUS "nxvc_transport AEAD backend: ${_nxt_crypto} (NullAead always available)")

if(NXWARP_BUILD_TOOLS OR NOT DEFINED NXWARP_BUILD_TOOLS)
    add_executable(nxvc-netsim
        sim/netsim.cpp
        sim/link.cpp
        sim/report.cpp
    )
    target_link_libraries(nxvc-netsim PRIVATE nxvc_transport)
    target_include_directories(nxvc-netsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sim)
    set_target_properties(nxvc-netsim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(nxvc-netsim PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()
