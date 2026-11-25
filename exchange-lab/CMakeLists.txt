cmake_minimum_required(VERSION 3.20)
project(exchange_lab LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
add_library(exchange_lab STATIC
    src/notional.cpp
    src/units.cpp
    src/model.cpp
    src/order_book.cpp
    src/engine.cpp
    src/ledger.cpp
    src/persistence.cpp
    src/snapshot.cpp
    src/futures_risk.cpp
)
target_compile_features(exchange_lab PUBLIC cxx_std_20)
target_include_directories(exchange_lab PUBLIC include)

add_executable(exchange_demo app/main.cpp)
target_link_libraries(exchange_demo PRIVATE exchange_lab)

enable_testing()
add_executable(exchange_tests tests/notional_test.cpp)
target_link_libraries(exchange_tests PRIVATE exchange_lab)
add_test(NAME quote_notional COMMAND exchange_tests)

add_executable(order_draft_tests tests/order_draft_test.cpp)
target_link_libraries(order_draft_tests PRIVATE exchange_lab)
add_test(NAME order_draft COMMAND order_draft_tests)

add_executable(lifetime_tests tests/lifetime_test.cpp)
target_link_libraries(lifetime_tests PRIVATE exchange_lab)
add_test(NAME lifetime COMMAND lifetime_tests
         "$<TARGET_FILE_DIR:lifetime_tests>/lifetime_test_output.txt")

add_executable(container_tests tests/containers_test.cpp)
target_link_libraries(container_tests PRIVATE exchange_lab)
add_test(NAME containers COMMAND container_tests)

add_executable(units_tests tests/units_test.cpp)
target_link_libraries(units_tests PRIVATE exchange_lab)
add_test(NAME units COMMAND units_tests)

add_executable(trading_rules_tests tests/trading_rules_test.cpp)
target_link_libraries(trading_rules_tests PRIVATE exchange_lab)
add_test(NAME trading_rules COMMAND trading_rules_tests)

add_executable(order_book_tests tests/order_book_test.cpp)
target_link_libraries(order_book_tests PRIVATE exchange_lab)
add_test(NAME order_book COMMAND order_book_tests)

add_executable(order_index_tests tests/order_index_test.cpp)
target_link_libraries(order_index_tests PRIVATE exchange_lab)
add_test(NAME order_index COMMAND order_index_tests)

add_executable(order_book_snapshot_tests tests/order_book_snapshot_test.cpp)
target_link_libraries(order_book_snapshot_tests PRIVATE exchange_lab)
add_test(NAME order_book_snapshot COMMAND order_book_snapshot_tests)

add_executable(gtc_tests tests/gtc_test.cpp)
target_link_libraries(gtc_tests PRIVATE exchange_lab)
add_test(NAME gtc COMMAND gtc_tests)

add_executable(ioc_tests tests/ioc_test.cpp)
target_link_libraries(ioc_tests PRIVATE exchange_lab)
add_test(NAME ioc COMMAND ioc_tests)

add_executable(budget_tests tests/budget_test.cpp)
target_link_libraries(budget_tests PRIVATE exchange_lab)
add_test(NAME budget COMMAND budget_tests)

add_executable(order_lifecycle_tests tests/order_lifecycle_test.cpp)
target_link_libraries(order_lifecycle_tests PRIVATE exchange_lab)
add_test(NAME order_lifecycle COMMAND order_lifecycle_tests)

add_executable(book_differential_tests tests/book_differential_test.cpp)
target_link_libraries(book_differential_tests PRIVATE exchange_lab)
add_test(NAME book_differential COMMAND book_differential_tests)

add_executable(engine_tests tests/engine_test.cpp)
target_link_libraries(engine_tests PRIVATE exchange_lab)
add_test(NAME engine COMMAND engine_tests)

add_executable(ledger_tests tests/ledger_test.cpp)
target_link_libraries(ledger_tests PRIVATE exchange_lab)
add_test(NAME ledger COMMAND ledger_tests)

add_executable(settlement_tests tests/settlement_test.cpp)
target_link_libraries(settlement_tests PRIVATE exchange_lab)
add_test(NAME settlement COMMAND settlement_tests)


add_executable(spot_integration_tests tests/spot_integration_test.cpp)
target_link_libraries(spot_integration_tests PRIVATE exchange_lab)
add_test(NAME spot_integration COMMAND spot_integration_tests)

add_executable(spot_demo app/spot_demo.cpp)
target_link_libraries(spot_demo PRIVATE exchange_lab)

add_executable(engine_bench bench/engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE exchange_lab)
add_test(NAME engine_bench_smoke COMMAND engine_bench mix)

add_executable(layout_probe bench/layout_probe.cpp)
target_link_libraries(layout_probe PRIVATE exchange_lab)
add_test(NAME layout_probe_smoke COMMAND layout_probe)

add_executable(order_node_pool_tests tests/order_node_pool_test.cpp)
target_link_libraries(order_node_pool_tests PRIVATE exchange_lab)
add_test(NAME order_node_pool COMMAND order_node_pool_tests)

add_executable(pooled_book_tests tests/pooled_book_test.cpp)
target_link_libraries(pooled_book_tests PRIVATE exchange_lab)
add_test(NAME pooled_book COMMAND pooled_book_tests)

find_package(Threads REQUIRED)
add_executable(bounded_queue_tests tests/bounded_queue_test.cpp)
target_link_libraries(bounded_queue_tests PRIVATE exchange_lab Threads::Threads)
add_test(NAME bounded_queue COMMAND bounded_queue_tests)
set_tests_properties(bounded_queue PROPERTIES TIMEOUT 15)

add_executable(spsc_tests tests/spsc_test.cpp)
target_link_libraries(spsc_tests PRIVATE exchange_lab Threads::Threads)
add_test(NAME spsc COMMAND spsc_tests)
set_tests_properties(spsc PROPERTIES TIMEOUT 15)

add_executable(async_engine_tests tests/async_engine_test.cpp)
target_link_libraries(async_engine_tests PRIVATE exchange_lab Threads::Threads)
add_test(NAME async_engine COMMAND async_engine_tests)
set_tests_properties(async_engine PROPERTIES TIMEOUT 20)

add_executable(journal_tests tests/journal_test.cpp)
target_link_libraries(journal_tests PRIVATE exchange_lab)
add_test(NAME journal COMMAND journal_tests)

add_executable(snapshot_tests tests/snapshot_test.cpp)
target_link_libraries(snapshot_tests PRIVATE exchange_lab)
add_test(NAME snapshot COMMAND snapshot_tests)

add_executable(recovery_lab app/recovery_lab.cpp)
target_link_libraries(recovery_lab PRIVATE exchange_lab Threads::Threads)
add_test(NAME recovery_matrix
    COMMAND ${CMAKE_COMMAND}
        "-DLAB=$<TARGET_FILE:recovery_lab>"
        "-DROOT=$<TARGET_FILE_DIR:recovery_lab>/recovery-matrix-data"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery_matrix.cmake")
set_tests_properties(recovery_matrix PROPERTIES TIMEOUT 30)

add_executable(futures_risk_tests tests/futures_risk_test.cpp)
target_link_libraries(futures_risk_tests PRIVATE exchange_lab)
add_test(NAME futures_risk COMMAND futures_risk_tests)

add_executable(futures_snapshot_tests tests/futures_snapshot_test.cpp)
target_link_libraries(futures_snapshot_tests PRIVATE exchange_lab)
add_test(NAME futures_snapshot COMMAND futures_snapshot_tests)

add_executable(integration_matrix_tests tests/integration_matrix_test.cpp)
target_link_libraries(integration_matrix_tests PRIVATE exchange_lab Threads::Threads)
add_test(NAME integration_matrix COMMAND integration_matrix_tests)
set_tests_properties(integration_matrix PROPERTIES TIMEOUT 60)

add_executable(futures_recovery_lab app/futures_recovery_lab.cpp)
target_include_directories(futures_recovery_lab PRIVATE tests)
target_link_libraries(futures_recovery_lab PRIVATE exchange_lab)
add_test(NAME futures_recovery_matrix
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:futures_recovery_lab>
            -DROOT=$<TARGET_FILE_DIR:futures_recovery_lab>/futures-recovery-tests
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/futures_recovery_matrix.cmake)
set_tests_properties(futures_recovery_matrix PROPERTIES TIMEOUT 60)
