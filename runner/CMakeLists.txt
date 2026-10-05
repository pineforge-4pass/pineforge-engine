# Dependencies belong only to the optional executable, not PineForge::pineforge.
find_package(SQLite3 REQUIRED)
find_package(CURL 8.14.1 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

option(PINEFORGE_LIVE_TSAN "Instrument all native runner targets with ThreadSanitizer" OFF)
if(PINEFORGE_LIVE_TSAN)
    if(PINEFORGE_ENABLE_SANITIZERS)
        message(FATAL_ERROR "Runner ThreadSanitizer must use a separate build from ASan/UBSan")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(PINEFORGE_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
add_library(pineforge_live_support STATIC store.cpp transport.cpp routing.cpp delivery.cpp service.cpp)
target_compile_features(pineforge_live_support PUBLIC cxx_std_17)
target_include_directories(pineforge_live_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(TARGET SQLite3::SQLite3)
    set(_pf_sqlite_target SQLite3::SQLite3)
else()
    set(_pf_sqlite_target SQLite::SQLite3)
endif()
target_link_libraries(pineforge_live_support PUBLIC ${_pf_sqlite_target} CURL::libcurl OpenSSL::Crypto Threads::Threads)
target_compile_options(pineforge_live_support PRIVATE -Wall -Wextra -Wpedantic)
add_library(pineforge_live_startup STATIC native_startup.cpp)
target_compile_features(pineforge_live_startup PUBLIC cxx_std_17)
target_include_directories(pineforge_live_startup PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(pineforge_live_startup PUBLIC pineforge_live_support PineForge::pineforge)
target_compile_options(pineforge_live_startup PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
add_executable(pineforge-live main.cpp)
target_include_directories(pineforge-live PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(pineforge-live PRIVATE pineforge_live_startup ${CMAKE_DL_LIBS})
target_compile_options(pineforge-live PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
install(TARGETS pineforge-live RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# examples/strategy.cpp derives from pineforge::source::PineStrategyHost, so
# it is the one example that a kernel-only build cannot compile.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_library(native_live_example MODULE examples/strategy.cpp)
    target_link_libraries(native_live_example PRIVATE PineForge::pineforge)
    target_compile_options(native_live_example PRIVATE -fvisibility=hidden -ffp-contract=off -UNDEBUG)
    set_target_properties(native_live_example PROPERTIES PREFIX "" OUTPUT_NAME "native-live-example")
else()
    message(STATUS
        "Live runner example strategy.cpp skipped: it is Pine-bound and "
        "PINEFORGE_BUILD_SOURCE_LAYER is OFF")
endif()
add_library(native_market_example MODULE
    ${PROJECT_SOURCE_DIR}/examples/native/native_market_strategy.cpp)
target_link_libraries(native_market_example PRIVATE PineForge::pineforge)
target_compile_options(native_market_example PRIVATE -fvisibility=hidden -ffp-contract=off)
set_target_properties(native_market_example PROPERTIES PREFIX "" OUTPUT_NAME "native-market-example")

add_library(native_selected_example MODULE
    ${PROJECT_SOURCE_DIR}/examples/native/native_selected_strategy.cpp)
target_link_libraries(native_selected_example PRIVATE PineForge::pineforge)
target_compile_options(native_selected_example PRIVATE -fvisibility=hidden -ffp-contract=off)
set_target_properties(native_selected_example PROPERTIES PREFIX "" OUTPUT_NAME "native-selected-example")

# Both modules are examples/native sources, which check their own results with
# assert(); test_native_example_batch and test_native_example_selected drive
# them through the C ABI. Keep assert() live here too, as
# examples/native/CMakeLists.txt does for their example_* executables: strip
# NDEBUG after the build-type flags, so it wins over Release's -DNDEBUG.
# scripts/ci_verify.py's examples-assert-live stage checks every compile of an
# examples/native source for it.
foreach(_pf_example_module native_market_example native_selected_example)
    target_compile_options(${_pf_example_module} PRIVATE -UNDEBUG)
endforeach()

if(PINEFORGE_BUILD_TESTS)
    add_executable(test_live_json ${PROJECT_SOURCE_DIR}/tests/test_live_json.cpp)
    target_include_directories(test_live_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(test_live_json PRIVATE -UNDEBUG)
    add_test(NAME test_live_json COMMAND test_live_json)
    add_executable(test_live_identity ${PROJECT_SOURCE_DIR}/tests/test_live_identity.cpp)
    target_link_libraries(test_live_identity PRIVATE pineforge_live_support)
    target_compile_options(test_live_identity PRIVATE -UNDEBUG)
    add_test(NAME test_live_identity COMMAND test_live_identity)
    add_executable(test_native_live_startup ${PROJECT_SOURCE_DIR}/tests/test_native_live_startup.cpp)
    target_link_libraries(test_native_live_startup PRIVATE pineforge_live_startup)
    target_compile_options(test_native_live_startup PRIVATE -UNDEBUG)
    add_test(NAME test_native_live_startup COMMAND test_native_live_startup)
    add_test(NAME native_live_help COMMAND pineforge-live --help)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_library(native_live_service_example MODULE ${PROJECT_SOURCE_DIR}/tests/native_live_service_strategy.cpp)
        target_link_libraries(native_live_service_example PRIVATE PineForge::pineforge)
        target_compile_options(native_live_service_example PRIVATE -fvisibility=hidden -ffp-contract=off -UNDEBUG)
        set_target_properties(native_live_service_example PROPERTIES PREFIX "")
        add_test(NAME native_live_service_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_service_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example> $<TARGET_FILE:native_live_service_example>)
        set_tests_properties(native_live_service_e2e PROPERTIES TIMEOUT 120)
        target_sources(native_live_example PRIVATE $<TARGET_OBJECTS:native_live_equivalence_observer>)
        set(_pf_report_strategies $<TARGET_FILE:native_live_example>)
        foreach(probe ta-rsi14-cross-50-01 order-entry-implicit-reversal-exit-01 order-tranche-partial-market-close-01 bracket-atr-trailing-stop-state-01)
            add_library(live_report_${probe} MODULE
                ${PROJECT_SOURCE_DIR}/corpus/validation/${probe}/generated.cpp
                $<TARGET_OBJECTS:native_live_equivalence_observer>)
            target_link_libraries(live_report_${probe} PRIVATE PineForge::pineforge)
            if(APPLE)
                target_link_options(live_report_${probe} PRIVATE "LINKER:-u,_pf_abi_version")
            else()
                target_link_options(live_report_${probe} PRIVATE "LINKER:-u,pf_abi_version")
            endif()
            target_compile_options(live_report_${probe} PRIVATE -ffp-contract=off)
            set_target_properties(live_report_${probe} PROPERTIES PREFIX "")
            list(APPEND _pf_report_strategies $<TARGET_FILE:live_report_${probe}>)
        endforeach()
        add_executable(native_live_report_oracle ${PROJECT_SOURCE_DIR}/tests/native_live_report_oracle.cpp)
        target_link_libraries(native_live_report_oracle PRIVATE pineforge_live_startup ${CMAKE_DL_LIBS})
        target_compile_options(native_live_report_oracle PRIVATE -ffp-contract=off)
        add_test(NAME native_live_report_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_report_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_report_oracle> ${_pf_report_strategies})
        set_tests_properties(native_live_report_e2e PROPERTIES TIMEOUT 300)
        add_test(NAME native_live_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example>)
        set_tests_properties(native_live_e2e PROPERTIES TIMEOUT 300)
    endif()
    foreach(contract_kind absent unknown native_no_export native_stub)
        add_library(live_contract_${contract_kind} MODULE
            ${PROJECT_SOURCE_DIR}/tests/test_live_contract_plugins.cpp)
        target_include_directories(live_contract_${contract_kind} PRIVATE
            ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
        target_compile_options(live_contract_${contract_kind} PRIVATE -fvisibility=hidden)
        set_target_properties(live_contract_${contract_kind} PROPERTIES PREFIX "")
    endforeach()
    target_compile_definitions(live_contract_absent PRIVATE PF_LIVE_CONTRACT_ABSENT=1)
    target_compile_definitions(live_contract_unknown PRIVATE PF_LIVE_CONTRACT_UNKNOWN=1)
    target_compile_definitions(live_contract_native_no_export PRIVATE PF_LIVE_CONTRACT_NATIVE_NO_EXPORT=1)
    target_compile_definitions(live_contract_native_stub PRIVATE PF_LIVE_CONTRACT_NATIVE_STUB=1)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_test(NAME native_live_startup_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_startup_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example> $<TARGET_FILE:native_market_example>
            $<TARGET_FILE:live_contract_absent> $<TARGET_FILE:live_contract_unknown>
            $<TARGET_FILE:live_contract_native_no_export> $<TARGET_FILE:live_contract_native_stub>)
        set_tests_properties(native_live_startup_e2e PROPERTIES TIMEOUT 120)
    endif()
    # Exercise the real MODULE through its C ABI, without linking a second engine.
    add_executable(test_native_example_batch
        ${PROJECT_SOURCE_DIR}/tests/test_native_example_batch.cpp)
    target_include_directories(test_native_example_batch PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
    target_compile_options(test_native_example_batch PRIVATE -UNDEBUG -ffp-contract=off)
    target_link_libraries(test_native_example_batch PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_native_example_batch native_market_example)
    add_test(NAME test_native_example_batch
        COMMAND test_native_example_batch $<TARGET_FILE:native_market_example>)
    set_tests_properties(test_native_example_batch PROPERTIES TIMEOUT 60)
    # Exercise the second native MODULE through the same C ABI boundary.
    add_executable(test_native_example_selected
        ${PROJECT_SOURCE_DIR}/tests/test_native_example_selected.cpp)
    target_include_directories(test_native_example_selected PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
    target_compile_options(test_native_example_selected PRIVATE -UNDEBUG -ffp-contract=off)
    target_link_libraries(test_native_example_selected PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_native_example_selected native_selected_example)
    add_test(NAME test_native_example_selected
        COMMAND test_native_example_selected $<TARGET_FILE:native_selected_example>)
    set_tests_properties(test_native_example_selected PROPERTIES TIMEOUT 60)
    add_library(pineforge_live_legacy_test_support STATIC store.cpp transport.cpp)
    target_compile_features(pineforge_live_legacy_test_support PUBLIC cxx_std_17)
    target_include_directories(pineforge_live_legacy_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(pineforge_live_legacy_test_support PUBLIC PINEFORGE_LIVE_LEGACY_TEST_API)
    target_link_libraries(pineforge_live_legacy_test_support PUBLIC
        ${_pf_sqlite_target} CURL::libcurl OpenSSL::Crypto Threads::Threads)
    foreach(native_test test_native_live_store test_native_live_websocket test_native_live_routing test_native_live_service)
        add_executable(${native_test} ${PROJECT_SOURCE_DIR}/tests/${native_test}.cpp)
        if(native_test STREQUAL "test_native_live_store")
            target_link_libraries(${native_test} PRIVATE pineforge_live_legacy_test_support)
        else()
            target_link_libraries(${native_test} PRIVATE pineforge_live_support)
        endif()
        target_compile_options(${native_test} PRIVATE -UNDEBUG)
        add_test(NAME ${native_test} COMMAND ${native_test})
        set_tests_properties(${native_test} PROPERTIES TIMEOUT 120)
    endforeach()
    set_tests_properties(test_native_live_websocket PROPERTIES SKIP_RETURN_CODE 77)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_test(NAME native_live_routing_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_routing_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example>)
        set_tests_properties(native_live_routing_e2e PROPERTIES TIMEOUT 300)
        add_test(NAME native_live_websocket_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_websocket_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example>)
        set_tests_properties(native_live_websocket_e2e PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()
endif()

if(PINEFORGE_BUILD_TESTS AND PINEFORGE_BUILD_SOURCE_LAYER)
    add_library(checked_settings_example MODULE $<TARGET_OBJECTS:checked_settings_fixture>)
    target_link_libraries(checked_settings_example PRIVATE PineForge::pineforge)
    if(APPLE)
        target_link_options(checked_settings_example PRIVATE "LINKER:-u,_pf_abi_version")
    else()
        target_link_options(checked_settings_example PRIVATE "LINKER:-u,pf_abi_version")
    endif()
    set_target_properties(checked_settings_example PROPERTIES PREFIX "")
    add_test(NAME checked_settings_e2e COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/checked_settings_e2e.py $<TARGET_FILE:pineforge-live>
        $<TARGET_FILE:checked_settings_example> $<TARGET_FILE:native_live_example>)
    set_tests_properties(checked_settings_e2e PROPERTIES TIMEOUT 120)
    foreach(capabilities_kind unknown)
        add_library(capabilities_${capabilities_kind} MODULE
            ${PROJECT_SOURCE_DIR}/tests/fixtures/strategy_capabilities.cpp
            $<TARGET_OBJECTS:checked_settings_fixture>)
        target_link_libraries(capabilities_${capabilities_kind} PRIVATE PineForge::pineforge)
        if(APPLE)
            target_link_options(capabilities_${capabilities_kind} PRIVATE "LINKER:-u,_pf_abi_version")
        else()
            target_link_options(capabilities_${capabilities_kind} PRIVATE "LINKER:-u,pf_abi_version")
        endif()
        set_target_properties(capabilities_${capabilities_kind} PROPERTIES PREFIX "")
    endforeach()
    target_compile_definitions(capabilities_unknown PRIVATE PF_TEST_CAPABILITIES_VERSION=2)
    set(capabilities_libraries)
    foreach(capabilities_kind close clock_flags tick fills pooc magnifier standard limits currency
            timeframe history_tick unresolved unresolved_tick varip realtime timenow islast
            lastconfirmedhistory last_bar_index last_bar_time security lower_tf recorded unpinned footprint positional)
        add_library(capabilities_${capabilities_kind} MODULE
            ${PROJECT_SOURCE_DIR}/tests/fixtures/strategy_capabilities/generated/${capabilities_kind}.cpp)
        target_link_libraries(capabilities_${capabilities_kind} PRIVATE PineForge::pineforge)
        target_compile_options(capabilities_${capabilities_kind} PRIVATE -ffp-contract=off)
        if(APPLE)
            target_link_options(capabilities_${capabilities_kind} PRIVATE "LINKER:-u,_pf_abi_version")
        else()
            target_link_options(capabilities_${capabilities_kind} PRIVATE "LINKER:-u,pf_abi_version")
        endif()
        set_target_properties(capabilities_${capabilities_kind} PROPERTIES PREFIX "")
        list(APPEND capabilities_libraries "${capabilities_kind}=$<TARGET_FILE:capabilities_${capabilities_kind}>")
    endforeach()
    add_test(NAME strategy_capabilities_e2e COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/strategy_capabilities_e2e.py $<TARGET_FILE:pineforge-live>
        $<TARGET_FILE:native_live_example> ${capabilities_libraries}
        "unknown=$<TARGET_FILE:capabilities_unknown>")
    set_tests_properties(strategy_capabilities_e2e PROPERTIES TIMEOUT 120)
    foreach(capabilities_kind close clock_flags)
        add_executable(test_capabilities_equivalence_${capabilities_kind}
            ${PROJECT_SOURCE_DIR}/tests/test_capabilities_equivalence.cpp
            ${PROJECT_SOURCE_DIR}/tests/fixtures/strategy_capabilities/generated/${capabilities_kind}.cpp)
        target_link_libraries(test_capabilities_equivalence_${capabilities_kind} PRIVATE PineForge::pineforge)
        target_compile_options(test_capabilities_equivalence_${capabilities_kind} PRIVATE -UNDEBUG -ffp-contract=off)
        add_test(NAME capabilities_equivalence_${capabilities_kind}
            COMMAND test_capabilities_equivalence_${capabilities_kind})
    endforeach()
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_executable(test_confirmed_capabilities ${PROJECT_SOURCE_DIR}/tests/test_confirmed_capabilities.cpp)
        target_include_directories(test_confirmed_capabilities PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(test_confirmed_capabilities PRIVATE cxx_std_17)
        target_compile_options(test_confirmed_capabilities PRIVATE -UNDEBUG)
        add_test(NAME confirmed_capabilities_policy COMMAND test_confirmed_capabilities)
        set(_pf_confirmed_cases htf5_close htf5_close_active htf15_sma htf15_sma_pooc htf60_close daily_close heikinashi5
            htf60_ema_gaps period_previous pooc_dual_stop pooc_limit_reversal pooc_slipped_short
            pooc_short_exit constant_daily helper5 heikinashi_alias5 varip pooc_market pooc_close pooc_close_entry
            pooc_close_short pooc_close_short_entry)
        file(GLOB _pf_confirmed_refusal_sources CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/tests/fixtures/confirmed_capabilities/generated/refused_*.cpp")
        set(_pf_confirmed_refusals)
        foreach(refusal_source IN LISTS _pf_confirmed_refusal_sources)
            get_filename_component(refusal_case "${refusal_source}" NAME_WE)
            list(APPEND _pf_confirmed_refusals "${refusal_case}")
        endforeach()
        set(_pf_confirmed_libraries)
        foreach(confirmed_case IN LISTS _pf_confirmed_cases _pf_confirmed_refusals)
            add_library(confirmed_${confirmed_case} MODULE
                ${PROJECT_SOURCE_DIR}/tests/fixtures/confirmed_capabilities/generated/${confirmed_case}.cpp
                $<TARGET_OBJECTS:native_live_equivalence_observer>)
            target_link_libraries(confirmed_${confirmed_case} PRIVATE PineForge::pineforge)
            target_compile_options(confirmed_${confirmed_case} PRIVATE -ffp-contract=off)
            if(APPLE)
                target_link_options(confirmed_${confirmed_case} PRIVATE "LINKER:-u,_pf_abi_version")
            else()
                target_link_options(confirmed_${confirmed_case} PRIVATE "LINKER:-u,pf_abi_version")
            endif()
            set_target_properties(confirmed_${confirmed_case} PROPERTIES PREFIX "")
            list(APPEND _pf_confirmed_libraries "${confirmed_case}=$<TARGET_FILE:confirmed_${confirmed_case}>")
        endforeach()
        set(_pf_confirmed_refusal_requests)
        set(_pf_confirmed_refusal_pooc)
        foreach(confirmed_library IN LISTS _pf_confirmed_libraries)
            if(confirmed_library MATCHES "^(pooc_|refused_pooc_|htf15_sma_pooc=)")
                list(APPEND _pf_confirmed_refusal_pooc "${confirmed_library}")
            else()
                list(APPEND _pf_confirmed_refusal_requests "${confirmed_library}")
            endif()
        endforeach()
        set(_pf_confirmed_refusal_timeout 180)
        if(PINEFORGE_ENABLE_SANITIZERS OR PINEFORGE_LIVE_TSAN)
            set(_pf_confirmed_refusal_timeout 600)
        endif()
        foreach(refusal_family requests pooc)
            set(refusal_libraries ${_pf_confirmed_refusal_${refusal_family}})
            list(LENGTH refusal_libraries refusal_library_count)
            set(refusal_offset 0)
            set(refusal_shard 1)
            while(refusal_offset LESS refusal_library_count)
                list(SUBLIST refusal_libraries ${refusal_offset} 8 refusal_shard_libraries)
                set(refusal_test "confirmed_capabilities_refusals_${refusal_family}_${refusal_shard}")
                add_test(NAME ${refusal_test} COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/confirmed_capabilities_refusals.py $<TARGET_FILE:pineforge-live>
                    ${refusal_shard_libraries})
                set_tests_properties(${refusal_test} PROPERTIES
                    TIMEOUT ${_pf_confirmed_refusal_timeout} LABELS confirmed_refusal_shard)
                math(EXPR refusal_offset "${refusal_offset} + 8")
                math(EXPR refusal_shard "${refusal_shard} + 1")
            endwhile()
        endforeach()
        foreach(receipt_variant ordinary request pooc missing)
            add_library(confirmed_receipt_${receipt_variant} MODULE
                ${PROJECT_SOURCE_DIR}/tests/fixtures/confirmed_capabilities/receipt_versions.cpp)
            target_link_libraries(confirmed_receipt_${receipt_variant} PRIVATE PineForge::pineforge)
            target_compile_definitions(confirmed_receipt_${receipt_variant} PRIVATE PF_RECEIPT_${receipt_variant}=1)
            if(APPLE)
                target_link_options(confirmed_receipt_${receipt_variant} PRIVATE "LINKER:-u,_pf_abi_version")
            else()
                target_link_options(confirmed_receipt_${receipt_variant} PRIVATE "LINKER:-u,pf_abi_version")
            endif()
            set_target_properties(confirmed_receipt_${receipt_variant} PROPERTIES PREFIX "")
        endforeach()
        add_test(NAME confirmed_capabilities_receipt_versions COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/confirmed_receipt_versions.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:confirmed_receipt_ordinary> $<TARGET_FILE:confirmed_receipt_request>
            $<TARGET_FILE:confirmed_receipt_pooc> $<TARGET_FILE:confirmed_receipt_missing>)
        foreach(confirmed_case IN LISTS _pf_confirmed_cases)
            if(confirmed_case MATCHES "^pooc_" OR confirmed_case STREQUAL "htf15_sma_pooc")
                add_test(NAME confirmed_capabilities_${confirmed_case}_refusal_e2e COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/confirmed_capabilities_refusals.py $<TARGET_FILE:pineforge-live>
                    "${confirmed_case}=$<TARGET_FILE:confirmed_${confirmed_case}>")
                set_tests_properties(confirmed_capabilities_${confirmed_case}_refusal_e2e PROPERTIES TIMEOUT 180)
            else()
                add_test(NAME confirmed_capabilities_${confirmed_case}_e2e COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/confirmed_capabilities_e2e.py $<TARGET_FILE:pineforge-live>
                    $<TARGET_FILE:native_live_report_oracle> ${confirmed_case} $<TARGET_FILE:confirmed_${confirmed_case}>)
                set_tests_properties(confirmed_capabilities_${confirmed_case}_e2e PROPERTIES TIMEOUT 900)
            endif()
            add_executable(test_confirmed_equivalence_${confirmed_case}
                ${PROJECT_SOURCE_DIR}/tests/test_stream_security_equivalence.cpp
                ${PROJECT_SOURCE_DIR}/tests/fixtures/confirmed_capabilities/generated/${confirmed_case}.cpp)
            target_link_libraries(test_confirmed_equivalence_${confirmed_case} PRIVATE PineForge::pineforge)
            target_compile_options(test_confirmed_equivalence_${confirmed_case} PRIVATE -UNDEBUG -ffp-contract=off)
            target_compile_definitions(test_confirmed_equivalence_${confirmed_case} PRIVATE PINEFORGE_SECURITY_CASE="${confirmed_case}")
            add_test(NAME confirmed_capabilities_${confirmed_case}_equivalence COMMAND test_confirmed_equivalence_${confirmed_case} 1 500)
            if(confirmed_case MATCHES "^pooc_" OR confirmed_case STREQUAL "htf15_sma_pooc")
                set_tests_properties(confirmed_capabilities_${confirmed_case}_equivalence PROPERTIES LABELS "future_admission")
            endif()
        endforeach()
        foreach(confirmed_control htf5_close_active htf60_close)
            if(confirmed_control STREQUAL "htf60_close")
                set(_pf_confirmed_clock 15)
            else()
                set(_pf_confirmed_clock 5)
            endif()
            add_test(NAME confirmed_capabilities_${confirmed_control}_tf${_pf_confirmed_clock}_e2e COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/confirmed_capabilities_e2e.py $<TARGET_FILE:pineforge-live>
                $<TARGET_FILE:native_live_report_oracle> ${confirmed_control} $<TARGET_FILE:confirmed_${confirmed_control}> ${_pf_confirmed_clock})
            set_tests_properties(confirmed_capabilities_${confirmed_control}_tf${_pf_confirmed_clock}_e2e PROPERTIES TIMEOUT 900)
            add_test(NAME confirmed_capabilities_${confirmed_control}_tf${_pf_confirmed_clock}_equivalence
                COMMAND test_confirmed_equivalence_${confirmed_control} ${_pf_confirmed_clock} 500)
        endforeach()
    endif()
endif()
