# xline — line-editing module
#
# Based on isocline v1.1.0 by Daan Leijen (MIT), vendored and being
# gradually re-skinned to xKit conventions. See NOTICE and
# LICENSE.isocline for upstream attribution.
#
# Build model (matches upstream default):
#   isocline.c is the single translation unit; it #includes every
#   other .c in this directory from within its
#   `#if !defined(IC_SEPARATE_OBJS)` block. We therefore compile only
#   isocline.c and mark the rest as HEADER_FILE_ONLY so IDEs still
#   see them but the compiler doesn't produce duplicate symbols.

set(XLINE_UMBRELLA_SOURCES
  async.c
  attr.c
  bbcode.c
  bbcode_colors.c
  completers.c
  completions.c
  debug.c
  editline.c
  editline_completion.c
  editline_help.c
  editline_history.c
  highlight.c
  history.c
  mem.c
  str.c
  stringbuf.c
  term.c
  term_color.c
  tty.c
  tty_esc.c
  undo.c
  unicode.c
  wcwidth.c)

set_source_files_properties(${XLINE_UMBRELLA_SOURCES}
  PROPERTIES HEADER_FILE_ONLY TRUE)

file(GLOB XLINE_HEADERS "*.h")

add_library(xline ${XK_LIB_TYPE}
  isocline.c
  ${XLINE_UMBRELLA_SOURCES}
  ${XLINE_HEADERS})

# Disable upstream debug printf hooks.
target_compile_definitions(xline PRIVATE IC_NO_DEBUG_MSG)

# Upstream ships with looser warning hygiene than xKit's -Werror.
# Shield isocline from the parent's -Werror so a benign upstream
# warning never breaks our build.
target_compile_options(xline PRIVATE
  -Wno-error
  -Wno-unused-function
  -Wno-variadic-macro-arguments-omitted
  -Wno-gnu-zero-variadic-macro-arguments)

set_target_properties(xline PROPERTIES POSITION_INDEPENDENT_CODE ON)

# xline is self-contained (stdlib + POSIX/Win32 Console only).
# It intentionally does NOT link xbase yet — commits 2/3 will
# introduce that dependency as we remove the mem_t indirection.

message(STATUS "xline: line-editing module enabled (isocline fork)")
