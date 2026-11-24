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
