# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles\\appOpen_Editor_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\appOpen_Editor_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe-mcp_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe-mcp_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_align_eval_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_align_eval_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_core_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_core_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_engine_test_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_engine_test_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_features_test_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_features_test_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_plugin_duotone_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_plugin_duotone_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_text_test_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_text_test_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\oe_v2_test_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\oe_v2_test_autogen.dir\\ParseCache.txt"
  "appOpen_Editor_autogen"
  "oe-mcp_autogen"
  "oe_align_eval_autogen"
  "oe_core_autogen"
  "oe_engine_test_autogen"
  "oe_features_test_autogen"
  "oe_plugin_duotone_autogen"
  "oe_text_test_autogen"
  "oe_v2_test_autogen"
  )
endif()
