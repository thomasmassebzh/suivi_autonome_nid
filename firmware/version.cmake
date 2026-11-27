# Version affichée au démarrage, dans le journal et sur les pages web :
# « <version>-<commit> » (ex. 0.2.0-57300b0), « -modifie » si des fichiers suivis
# par Git ont été modifiés localement. Partagée par le firmware et le banc.
# La version est relue à chaque changement de commit (pull, checkout, commit).
set(NID_VERSION_BASE "0.2.0")

set(PROJECT_VER "${NID_VERSION_BASE}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE _nid_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _nid_git_res ERROR_QUIET)
    if(_nid_git_res EQUAL 0 AND _nid_commit)
        set(PROJECT_VER "${NID_VERSION_BASE}-${_nid_commit}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD -- .
                        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                        RESULT_VARIABLE _nid_dirty ERROR_QUIET)
        if(NOT _nid_dirty EQUAL 0)
            set(PROJECT_VER "${PROJECT_VER}-modifie")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
                        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                        OUTPUT_VARIABLE _nid_git_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(EXISTS "${_nid_git_dir}/logs/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nid_git_dir}/logs/HEAD")
        endif()
    endif()
endif()
message(STATUS "Suivi nid : version ${PROJECT_VER}")
