file(GLOB HEADERS "include/eosio/chain_ws_plugin/*.hpp")
add_library( chain_ws_plugin
             chain_ws_plugin.cpp
             ${HEADERS} )

target_link_libraries( chain_ws_plugin chain_plugin http_plugin eosio_chain appbase fc )
target_include_directories( chain_ws_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
