file(GLOB HEADERS "include/eosio/websocket_rpc_plugin/*.hpp")
add_library( websocket_rpc_plugin
             websocket_rpc_plugin.cpp
             ${HEADERS} )

target_link_libraries( websocket_rpc_plugin chain_plugin http_plugin eosio_chain appbase fc )
target_include_directories( websocket_rpc_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
