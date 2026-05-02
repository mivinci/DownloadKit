# xline — line-editing module
#
# Based on isocline v1.1.0 by Daan Leijen (MIT), vendored and being
# gradually re-skinned to xKit conventions. See NOTICE and
# LICENSE.isocline for upstream attribution.
#
# Build model:
#   Every .c file compiles as its own translation unit. We set
#   IC_SEPARATE_OBJS so upstream's ic_private expands to
#   __attribute__((visibility("hidden"))) instead of `static`, letting
#   internal helpers cross TU boundaries while still staying out of the
#   shared library's public symbol table.

set(XLINE_SOURCES
  async.c
  attr.c
  bbcode.c
  bbcode_colors.c
  completers.c
  completions.c
  edit.c
  edit_completion.c
  edit_help.c
  edit_history.c
  highlight.c
  history.c
  line.c
  str.c
  stringbuf.c
  term.c
  term_color.c
  tty.c
  tty_esc.c
  undo.c
  unicode.c
  wcwidth.c)

file(GLOB XLINE_HEADERS "*.h")

add_library(xline ${XK_LIB_TYPE}
  ${XLINE_SOURCES}
  ${XLINE_HEADERS})

# Separate-object build: every top-level .c is its own TU. ic_private
# becomes hidden-visibility instead of `static`, so cross-file helpers
# link cleanly without leaking into the shared library's exported
# symbols.
target_compile_definitions(xline PRIVATE
  IC_SEPARATE_OBJS
  _XOPEN_SOURCE=700        # wcwidth
  _DEFAULT_SOURCE          # keep usleep visible alongside _XOPEN_SOURCE>=700
  _CRT_NONSTDC_NO_WARNINGS # MSVC
  _CRT_SECURE_NO_WARNINGS) # MSVC

# Upstream ships with looser warning hygiene than xKit's -Werror.
# Shield isocline from the parent's -Werror so a benign upstream
# warning never breaks our build.
target_compile_options(xline PRIVATE
  -Wno-error
  -Wno-variadic-macro-arguments-omitted
  -Wno-gnu-zero-variadic-macro-arguments)

set_target_properties(xline PROPERTIES POSITION_INDEPENDENT_CODE ON)

# xline depends on xbase for logging.
target_link_libraries(xline PRIVATE xbase)

# xline is self-contained (stdlib + POSIX/Win32 Console only).

message(STATUS "xline: line-editing module enabled (isocline fork)")
