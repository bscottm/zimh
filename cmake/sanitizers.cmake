##~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
## Santizer support
##
## Enable compiler and linker sanitizer options, enforcing The Highlander (Connor McCleod) Rule.
##
## SANITIZE_ADDRESS: Address sanitizer
## SANITIZE_THREAD: Thread sanitizer
## SANITIZE_MEMORY: Memory sanitizer
## SANITIZE_UNDEFINED: Undefined behavior santizer
##
## Note: Options are declared in the top-level CMakeLists.txt.
##~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

# Enforce The Highlander (Connor McCleod) Rule: There can be only one!
set(_active_sanitizers 0)

if(SANITIZE_ADDRESS)
    math(EXPR _active_sanitizers "${_active_sanitizers} + 1")
endif()

if(SANITIZE_THREAD)
    math(EXPR _active_sanitizers "${_active_sanitizers} + 1")
endif()

if(SANITIZE_MEMORY)
    math(EXPR _active_sanitizers "${_active_sanitizers} + 1")
endif()

# (Note: UBSan is excluded from the count because it can coexist with ASan)

if(_active_sanitizers GREATER 1)
    message(FATAL_ERROR
        "Highlander Rule Violation: There can be only one major sanitizer active at a time! "
        "Please choose only ONE of: SANITIZE_ADDRESS, SANITIZE_THREAD, or SANITIZE_MEMORY."
    )
endif()

# sanitizer_options: Interface library that packages the requested sanitizer.
add_library(sanitizer_options INTERFACE)

# The user can only enable one of these major sanitizers at a time
if(SANITIZE_ADDRESS)
    if(CMAKE_C_COMPILER_ID MATCHES "MSVC")
        target_compile_options(sanitizer_options INTERFACE /fsanitize=address)
        target_link_options(sanitizer_options INTERFACE /fsanitize=address)
    else()
        target_compile_options(sanitizer_options INTERFACE -g -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(sanitizer_options INTERFACE -fsanitize=address)
    endif()

elseif(SANITIZE_THREAD)
    target_compile_options(sanitizer_options INTERFACE -g -fsanitize=thread)
    target_link_options(sanitizer_options INTERFACE -fsanitize=thread)

elseif(SANITIZE_MEMORY)
    target_compile_options(sanitizer_options INTERFACE -g -fsanitize=memory)
    target_link_options(sanitizer_options INTERFACE -fsanitize=memory)

    # The memory sanitizer relies on position-independent executables.
    set_target_properties(sanitizer_options PROPERTIES
        INTERFACE_POSITION_INDEPENDENT_CODE ON
    )
endif()

# UndefinedBehaviorSanitizer (UBSan) CAN be combined with AddressSanitizer, so it gets its own independent
# check block.
if(SANITIZE_UNDEFINED AND NOT CMAKE_C_COMPILER_ID MATCHES "MSVC")
    target_compile_options(sanitizer_options INTERFACE -fsanitize=undefined)
    target_link_options(sanitizer_options INTERFACE -fsanitize=undefined)
endif()
