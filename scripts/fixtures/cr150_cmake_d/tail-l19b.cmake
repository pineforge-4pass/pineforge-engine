# --- CR-150 core-cr150-l19b begin ---
# CR-150 L19B: match the library Debug-mode precedent for
# test_native_runtime_ambient_lifo at tests/CMakeLists.txt:4129-4130.
target_compile_definitions(test_group_receipt_retirement PRIVATE PINEFORGE_LIBRARY_DEBUG_CHECKS=$<CONFIG:Debug>)
# --- CR-150 core-cr150-l19b end ---
