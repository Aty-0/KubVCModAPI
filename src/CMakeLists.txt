add_library(kub_mod_api INTERFACE)
add_library(kub::mod_api ALIAS kub_mod_api)

target_include_directories(kub_mod_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})