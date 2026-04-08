function(xk_option key hint value)
  option(${key} "${hint}" ${value})

  if(${key})
    # remove prefix 'XK_' from key for definition
    string(SUBSTRING ${key} 3 -1 key)
    add_definitions(-D${key})
    message(STATUS "Option ${key} ON")
  endif()
endfunction(xk_option)


function(xk_version path)
  if(NOT EXISTS ${path})
    message(FATAL_ERROR "Version file not found: ${path}")
  endif()

  file(READ ${path} version_content)
  string(STRIP "${version_content}" version_string)

  # Format: MAJOR.MINOR.PATCH[-CHANNEL.BUILDNO][+META]
  # e.g. 0.0.1-alpha.1+main 或 1.2.3 或 2.0.0-beta.2
  if(NOT version_string MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([a-zA-Z]+)\\.([0-9]+))?([+][^+]+)?$")
    message(FATAL_ERROR "Invalid version format: ${version_string}")
  endif()

  set(XK_VERSION_MAJOR ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(XK_VERSION_MINOR ${CMAKE_MATCH_2} PARENT_SCOPE)
  set(XK_VERSION_PATCH ${CMAKE_MATCH_3} PARENT_SCOPE)
  
  # Channel e.g. alpha, beta, rc
  set(XK_VERSION_CHANNEL ${CMAKE_MATCH_5} PARENT_SCOPE)
  
  # BuildNo
  set(XK_VERSION_BUILDNO ${CMAKE_MATCH_6} PARENT_SCOPE)
  
  # Meta e.g. branch name
  string(REGEX REPLACE "^\\+" "" XK_VERSION_META "${CMAKE_MATCH_7}")
  set(XK_VERSION_META ${XK_VERSION_META} PARENT_SCOPE)
  
  # Full version string
  set(XK_VERSION ${version_string} PARENT_SCOPE)

  add_definitions(-DXK_VERSION_MAJOR=${XK_VERSION_MAJOR})
  add_definitions(-DXK_VERSION_MINOR=${XK_VERSION_MINOR})
  add_definitions(-DXK_VERSION_PATCH=${XK_VERSION_PATCH})
  
  if(XK_VERSION_CHANNEL)
    add_definitions(-DXK_VERSION_CHANNEL="${XK_VERSION_CHANNEL}")
  endif()
  
  if(XK_VERSION_BUILDNO)
    add_definitions(-DXK_VERSION_BUILDNO=${XK_VERSION_BUILDNO})
  endif()
  
  if(XK_VERSION_META)
    add_definitions(-DXK_VERSION_META="${XK_VERSION_META}")
  endif()
  
  add_definitions(-DXK_VERSION="${XK_VERSION}")
endfunction(xk_version)

function(xk_add_benchmark name)
  cmake_parse_arguments(BENCH "" "" "SOURCES;LIBS" ${ARGN})

  add_executable(${name} ${BENCH_SOURCES})
  target_link_libraries(${name} PRIVATE
    ${BENCH_LIBS}
    GBenchmark::benchmark
    GBenchmark::benchmark_main
  )
  # Disable -Werror for benchmark C++ code
  target_compile_options(${name} PRIVATE -Wno-error)
endfunction()

function(xk_message message)
  message("** ${message}")
endfunction(xk_message)
