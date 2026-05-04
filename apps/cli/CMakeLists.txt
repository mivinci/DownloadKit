# xKit command-line tool.
#
# Canonical end-to-end integration of the xagent stack
# (xAgent -> xAgentSession -> xAgentTool -> xAgentProvider) wrapped
# in an xline-powered async REPL. See apps/cli/main.cpp for the
# translation-unit map and overall data flow.
#
# Reaches into xagent/session_private.h to print a one-line
# budget-calibrator snapshot in on_done (see the file-header
# comment in main.cpp). That is an intentional diagnostic escape
# hatch — the calibrator has no public accessor by design
# (session-internal diagnostics, see session_test.cpp's
# BudgetCalibrator suite) and we'd rather keep the surface small
# than export a getter that exists only to feed a cli.
add_executable(cli
  main.cpp
  output.cpp
  slash.cpp
  callbacks.cpp
  repl.cpp
)
target_link_libraries(cli PRIVATE xagent xhttp xbase xline)
target_include_directories(cli PRIVATE
  ${CMAKE_SOURCE_DIR}/modules
  ${CMAKE_CURRENT_SOURCE_DIR}
)
