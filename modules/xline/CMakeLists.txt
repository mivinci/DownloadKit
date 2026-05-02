# xline — line-editing module
#
# Based on isocline v1.1.0 by Daan Leijen (MIT), vendored and being
# gradually re-skinned to xKit conventions. See NOTICE and
# LICENSE.isocline for upstream attribution.
#
# Build model:
#   Every top-level .c compiles as its own translation unit. We set
#   IC_SEPARATE_OBJS so upstream's ic_private expands to
#   __attribute__((visibility("hidden"))) instead of `static`, letting
#   internal helpers cross TU boundaries while still staying out of the
#   shared library's public symbol table.
#
#   A handful of upstream files are designed to be #included into a
#   parent .c (e.g. term_color.c inside term.c, editline_help.c inside
#   editline.c, wcwidth.c inside stringbuf.c). Those are marked as
#   HEADER_FILE_ONLY so the compiler does not produce duplicate object
#   files for them while IDEs still see them in the project.

set(XLINE_TOPLEVEL_SOURCES
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
  isocline.c
  mem.c
  str.c
  stringbuf.c
  term.c
  tty.c
  tty_esc.c
  undo.c
  unicode.c
  wcwidth.c)

# Partial translation units that are #included into a parent .c.
# Kept in the target for IDE visibility but not compiled directly.
set(XLINE_PARTIAL_SOURCES
  term_color.c)

set_source_files_properties(${XLINE_PARTIAL_SOURCES}
  PROPERTIES HEADER_FILE_ONLY TRUE)

file(GLOB XLINE_HEADERS "*.h")

add_library(xline ${XK_LIB_TYPE}
  ${XLINE_TOPLEVEL_SOURCES}
  ${XLINE_PARTIAL_SOURCES}
  ${XLINE_HEADERS})

# Separate-object build: every top-level .c is its own TU. ic_private
# becomes hidden-visibility instead of `static`, so cross-file helpers
# link cleanly without leaking into the shared library's exported
# symbols.
target_compile_definitions(xline PRIVATE
  IC_SEPARATE_OBJS
  IC_NO_DEBUG_MSG
  _XOPEN_SOURCE=700        # wcwidth
  _DEFAULT_SOURCE          # keep usleep visible alongside _XOPEN_SOURCE>=700
  _CRT_NONSTDC_NO_WARNINGS # MSVC
  _CRT_SECURE_NO_WARNINGS) # MSVC

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

message(STATUS "xline: line-editing module enabled (isocline fork)")
