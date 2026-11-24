# macOS only: dsperate-macos.patch makes DSperate (third_party/dsperate-lr/DSperate) build with Apple clang and Mach-O
# (see the patch's header). Applied to the submodule's working tree at configure time - once: a tree that already has it
# is left alone (looked for by its text) - so a fresh checkout builds as is. Linux, Windows and the handhelds never apply it.
# Moving the submodule means re-making the patch (git -C third_party/dsperate-lr/DSperate diff, below the header).
#   include(.../apply_dsperate_macos_patch.cmake) after setting DS (the DSperate directory)
set(PK_DSMAC_PATCH "${CMAKE_CURRENT_LIST_DIR}/dsperate-macos.patch")
file(READ "${DS}/src/core/cheat/analog_db.cpp" PK_DSMAC_ANALOG)
file(READ "${DS}/src/core/crypto/aes.c" PK_DSMAC_AES)
string(FIND "${PK_DSMAC_ANALOG}" "DS_ASM_RODATA" PK_DSMAC_PATCHED)
string(FIND "${PK_DSMAC_AES}" "every Apple Silicon chip has the AES instructions" PK_DSMAC_PATCHED_AES)
if(PK_DSMAC_PATCHED EQUAL -1 AND PK_DSMAC_PATCHED_AES EQUAL -1)
    execute_process(COMMAND git apply "${PK_DSMAC_PATCH}"
        WORKING_DIRECTORY "${DS}" RESULT_VARIABLE PK_DSMAC_APPLY ERROR_VARIABLE PK_DSMAC_ERR)
    if(NOT PK_DSMAC_APPLY EQUAL 0)
        message(FATAL_ERROR "DSperate macOS patch didn't apply (${PK_DSMAC_PATCH}): ${PK_DSMAC_ERR}")
    endif()
    message(STATUS "DSperate: applied dsperate-macos.patch")
elseif(PK_DSMAC_PATCHED EQUAL -1 OR PK_DSMAC_PATCHED_AES EQUAL -1)
    message(FATAL_ERROR "DSperate holds part of dsperate-macos.patch: git -C third_party/dsperate-lr/DSperate checkout . and configure again")
endif()
