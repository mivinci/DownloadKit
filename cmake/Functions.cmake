# Construct a GitHub archive URL, optionally through a mirror.
#
# Usage:
#   xk_github_url(<owner>/<repo> <ref> <out-var>)
#
# Produces:
#   https://github.com/<owner>/<repo>/archive/<ref>.tar.gz
#
# <ref> can be a tag (e.g. v1.15.2) or a branch (e.g. master).
#
# If GITHUB_MIRROR is set (e.g. "https://ghfast.top/https://github.com"),
# the URL prefix "https://github.com" is replaced with the mirror value.
function(xk_github_url repo_path ref out_var)
  set(_url "https://github.com/${repo_path}/archive/${ref}.tar.gz")
  if(DEFINED ENV{GITHUB_MIRROR} AND NOT "$ENV{GITHUB_MIRROR}" STREQUAL "")
    string(REPLACE "https://github.com" "$ENV{GITHUB_MIRROR}" _url "${_url}")
  endif()
  set(${out_var} "${_url}" PARENT_SCOPE)
endfunction()

function(xk_add_benchmark name)
  cmake_parse_arguments(BENCH "" "" "SOURCES;LIBS" ${ARGN})

  add_executable(${name} ${BENCH_SOURCES})
  target_link_libraries(${name} PRIVATE
    ${BENCH_LIBS}
    GBenchmark::benchmark_main
  )
  # Disable -Werror for benchmark C++ code
  target_compile_options(${name} PRIVATE -Wno-error)
endfunction()
