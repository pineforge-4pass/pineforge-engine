# Include after creating a strategy module, then call
# pineforge_attach_hpo_bridge(<that module>). This compiles against its SAME
# engine/header closure; loading a second runtime DSO beside it is unsafe.
function(pineforge_attach_hpo_bridge strategy_target)
    target_sources(${strategy_target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/hpo_bridge.cpp")
endfunction()
