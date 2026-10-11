# Confirmed-capabilities refusal registration: discover the generated
# refused_*.cpp fixtures that the confirmed-capabilities rows register.
#
# Included from runner/CMakeLists.txt at the position the discovery held
# before, inside the same guard. This is a move, not a rule change: the
# variable name, the glob pattern and the ordering are unchanged, so the same
# sources register in the same order. The consumers (`set(_pf_confirmed_refusals)`
# and its `foreach()`) stay in runner/CMakeLists.txt, downstream of this include.
file(GLOB _pf_confirmed_refusal_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tests/fixtures/confirmed_capabilities/generated/refused_*.cpp")
