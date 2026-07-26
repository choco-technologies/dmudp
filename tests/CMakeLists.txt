set(DMOD_MODULE_NAME        test_dmudp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmudp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# dmarp: ARP cache entries are seeded by hand so dmudp_send()'s full
#        pipeline (through dmip) can run without a real driver - see
#        dmudp_test.c's file comment.
# dmnetbridge: feed_udp_packet() drives dmip's packet_received DIF
#              implementation directly via Dmod_GetNextDifModule()/
#              Dmod_GetDifFunction(), the same way
#              dmnetbridge_handle_netif_rx() itself does.
target_link_libraries(${DMOD_MODULE_NAME} dmudp_if)

# dmnetif: fixture interfaces are seeded by hand - see dmudp_test.c's file
#          comment.
# dmroute: routes are seeded by hand - see dmudp_test.c's file comment.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmnetbridge
    dmarp
    dmnetif
    dmroute
)
