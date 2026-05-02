/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef XLINE_LINE_H
#define XLINE_LINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>    // size_t
#include <stdbool.h>   // bool
#include <stdint.h>    // uint32_t
#include <stdarg.h>    // term_vprintf


/*! \mainpage
Isocline C API reference.

Isocline is a pure C library that can be used as an alternative to the GNU readline library.

See the [Github repository](https://github.com/daanx/isocline#readme) 
for general information and building the library.

Contents:
- \ref readline
- \ref bbcode
- \ref history
- \ref completion
- \ref highlight
- \ref options
- \ref helper
- \ref completex
- \ref term
- \ref async
- \ref alloc
*/

/// \defgroup readline Readline
/// The basic readline interface.
/// \{

/// Isocline version: 110 = 1.1.0.
#define XLINE_VERSION   (110)  


/// Read input from the user using rich editing abilities.
/// @param prompt_text   The prompt text, can be NULL for the default (""). 
///   The displayed prompt becomes `prompt_text` followed by the `prompt_marker` ("> "). 
/// @returns the heap allocated input on succes, which should be `free`d by the caller.  
///   Returns NULL on error, or if the user typed ctrl+d or ctrl+c.
///
/// If the standard input (`stdin`) has no editing capability 
/// (like a dumb terminal (e.g. `TERM`=`dumb`), running in a debuggen, a pipe or redirected file, etc.)
/// the input is read directly from the input stream up to the 
/// next line without editing capability.
/// See also \a xLineSetPromptMarker(), \a xLineStyleDef()
///
/// @see xLineSetPromptMarker(), xLineStyleDef()
char* xLineReadline(const char* prompt_text);   

/// \}


//--------------------------------------------------------------
/// \defgroup bbcode Formatted Text
/// Formatted text using [bbcode markup](https://github.com/daanx/isocline#bbcode-format).
/// \{

/// Print to the terminal while respection bbcode markup. 
/// Any unclosed tags are closed automatically at the end of the print.
/// For example:
/// ```
/// xLinePrint("[b]bold, [i]bold and italic[/i], [red]red and bold[/][/b] default.");
/// xLinePrint("[b]bold[/], [i b]bold and italic[/], [yellow on blue]yellow on blue background");
/// ic_style_add("em","i color=#888800");
/// xLinePrint("[em]emphasis");
/// ```
/// Properties that can be assigned are:
/// * `color=` _clr_, `bgcolor=` _clr_: where _clr_ is either a hex value `#`RRGGBB or `#`RGB, a
///    standard HTML color name, or an ANSI palette name, like `ansi-maroon`, `ansi-default`, etc.
/// * `bold`,`italic`,`reverse`,`underline`: can be `on` or `off`. 
/// * everything else is a style; all HTML and ANSI color names are also a style (so we can just use `red`
///   instead of `color=red`, or `on red` instead of `bgcolor=red`), and there are
///   the `b`, `i`, `u`, and `r` styles for bold, italic, underline, and reverse.
/// 
/// See [here](https://github.com/daanx/isocline#bbcode-format) for a description of the full bbcode format.
void xLinePrint( const char* s );

/// Print with bbcode markup ending with a newline.
/// @see xLinePrint()
void xLinePrintln( const char* s );

/// Print formatted with bbcode markup.
/// @see xLinePrint()
void xLinePrintf(const char* fmt, ...);

/// Print formatted with bbcode markup.
/// @see xLinePrint
void xLineVprintf(const char* fmt, va_list args);

/// Define or redefine a style.
/// @param style_name The name of the style. 
/// @param fmt        The `fmt` string is the content of a tag and can contain
///   other styles. This is very useful to theme the output of a program
///   by assigning standard styles like `em` or `warning` etc.
void xLineStyleDef( const char* style_name, const char* fmt );

/// Start a global style that is only reset when calling a matching xLineStyleClose().
void xLineStyleOpen( const char* fmt );

/// End a global style.
void xLineStyleClose(void);

/// \}


//--------------------------------------------------------------
// History
//--------------------------------------------------------------
/// \defgroup history History
/// Readline input history.
/// \{

/// Enable history. 
/// Use a \a NULL filename to not persist the history. Use -1 for max_entries to get the default (200).
void xLineSetHistory(const char* fname, long max_entries );

/// Remove the last entry in the history. 
/// The last returned input from xLineReadline() is automatically added to the history; this function removes it.
void xLineHistoryRemoveLast(void);

/// Clear the history.
void xLineHistoryClear(void);

/// Add an entry to the history
void xLineHistoryAdd( const char* entry );

/// \}

//--------------------------------------------------------------
// Basic Completion
//--------------------------------------------------------------

/// \defgroup completion Completion
/// Basic word completion.
/// \{

/// A completion environment
struct xLineCompletionEnvS;

/// A completion environment
typedef struct xLineCompletionEnvS xLineCompletionEnv;

/// A completion callback that is called by isocline when tab is pressed.
/// It is passed a completion environment (containing the current input and the current cursor position), 
/// the current input up-to the cursor (`prefix`)
/// and the user given argument when the callback was set.
/// When using completion transformers, like `ic_complete_quoted_word` the `prefix` contains the
/// the word to be completed without escape characters or quotes.
typedef void (xLineCompleterFn)(xLineCompletionEnv* cenv, const char* prefix );

/// Set the default completion handler.
/// @param completer  The completion function
/// @param arg        Argument passed to the \a completer.
/// There can only be one default completion function, setting it again disables the previous one.
/// The initial completer use `xLineCompleteFilename`.
void xLineSetDefaultCompleter( xLineCompleterFn* completer, void* arg);


/// In a completion callback (usually from xLineCompleteWord()), use this function to add a completion.
/// (the completion string is copied by isocline and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible completions.
/// If `false` is returned, the callback should try to return and not add more completions (for improved latency).
bool xLineAddCompletion(xLineCompletionEnv* cenv, const char* completion);

/// In a completion callback (usually from xLineCompleteWord()), use this function to add a completion.
/// The `display` is used to display the completion in the completion menu, and `help` is
/// displayed for hints for example. Both can be `NULL` for the default.
/// (all are copied by isocline and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible completions.
/// If `false` is returned, the callback should try to return and not add more completions (for improved latency).
bool xLineAddCompletionEx( xLineCompletionEnv* cenv, const char* completion, const char* display, const char* help );

/// In a completion callback (usually from xLineCompleteWord()), use this function to add completions.
/// The `completions` array should be terminated with a NULL element, and all elements
/// are added as completions if they start with `prefix`.
///
/// Returns `true` if the callback should continue trying to find more possible completions.
/// If `false` is returned, the callback should try to return and not add more completions (for improved latency).
bool xLineAddCompletions(xLineCompletionEnv* cenv, const char* prefix, const char** completions);

/// Complete a filename.
/// Complete a filename given a semi-colon separated list of root directories `roots` and 
/// semi-colon separated list of possible extensions (excluding directories). 
/// If `roots` is NULL, the current directory is the root ("."). 
/// If `extensions` is NULL, any extension will match.
/// Each root directory should _not_ end with a directory separator.
/// If a directory is completed, the `dir_separator` is added at the end if it is not `0`.
/// Usually the `dir_separator` is `/` but it can be set to `\\` on Windows systems.
/// For example:
/// ```
/// /ho         --> /home/
/// /home/.ba   --> /home/.bashrc
/// ```
/// (This already uses ic_complete_quoted_word() so do not call it from inside a word handler).
void xLineCompleteFilename( xLineCompletionEnv* cenv, const char* prefix, char dir_separator, const char* roots, const char* extensions );



/// Function that returns whether a (utf8) character (of length `len`) is in a certain character class
/// @see xLineCharIsSeparator() etc.
typedef bool (xLineIsCharClassFn)(const char* s, long len);


/// Complete a _word_ (i.e. _token_). 
/// Calls the user provided function `fun` to complete on the
/// current _word_. Almost all user provided completers should use this function. 
/// If `is_word_char` is NULL, the default `&xLineCharIsNonseparator` is used. 
/// The `prefix` passed to `fun` is modified to only contain the current word, and 
/// any results from `xLineAddCompletion` are automatically adjusted to replace that part.
/// For example, on the input "hello w", a the user `fun` only gets `w` and can just complete
/// with "world" resulting in "hello world" without needing to consider `delete_before` etc.
/// @see xLineCompleteQword() for completing quoted and escaped tokens.
void xLineCompleteWord(xLineCompletionEnv* cenv, const char* prefix, xLineCompleterFn* fun, xLineIsCharClassFn* is_word_char);


/// Complete a quoted _word_. 
/// Calls the user provided function `fun` to complete while taking
/// care of quotes and escape characters. Almost all user provided completers should use
/// this function. The `prefix` passed to `fun` is modified to be unquoted and unescaped, and 
/// any results from `xLineAddCompletion` are automatically quoted and escaped again.
/// For example, completing `hello world`, the `fun` always just completes `hel` or `hello w` to `hello world`, 
/// but depending on user input, it will complete as:
/// ```
/// hel        -->  hello\ world
/// hello\ w   -->  hello\ world
/// hello w    -->                   # no completion, the word is just 'w'>
/// "hel       -->  "hello world" 
/// "hello w   -->  "hello world"
/// ```
/// with proper quotes and escapes.
/// If `is_word_char` is NULL, the default `&xLineCharIsNonseparator` is used. 
/// @see ic_complete_quoted_word() to customize the word boundary, quotes etc.
void xLineCompleteQword( xLineCompletionEnv* cenv, const char* prefix, xLineCompleterFn* fun, xLineIsCharClassFn* is_word_char );



/// Complete a _word_. 
/// Calls the user provided function `fun` to complete while taking
/// care of quotes and escape characters. Almost all user provided completers should use this function. 
/// The `is_word_char` is a set of characters that are part of a "word". Use NULL for the default (`&xLineCharIsNonseparator`).
/// The `escape_char` is the escaping character, usually `\` but use 0 to not have escape characters.
/// The `quote_chars` define the quotes, use NULL for the default `"\'\""` quotes.
/// @see xLineCompleteWord() which uses the default values for `non_word_chars`, `quote_chars` and `\` for escape characters.
void xLineCompleteQwordEx( xLineCompletionEnv* cenv, const char* prefix, xLineCompleterFn fun, 
                                xLineIsCharClassFn* is_word_char, char escape_char, const char* quote_chars );

/// \}

//--------------------------------------------------------------
/// \defgroup highlight Syntax Highlighting
/// Basic syntax highlighting.
/// \{

/// A syntax highlight environment
struct xLineHighlightEnvS;
typedef struct xLineHighlightEnvS xLineHighlightEnv;

/// A syntax highlighter callback that is called by readline to syntax highlight user input.
typedef void (xLineHighlightFn)(xLineHighlightEnv* henv, const char* input, void* arg);

/// Set a syntax highlighter.
/// There can only be one highlight function, setting it again disables the previous one.
void xLineSetDefaultHighlighter(xLineHighlightFn* highlighter, void* arg);

/// Set the style of characters starting at position `pos`.
void xLineHighlight(xLineHighlightEnv* henv, long pos, long count, const char* style );

/// Experimental: Convenience callback for a function that highlights `s` using bbcode's.
/// The returned string should be allocated and is free'd by the caller.
typedef char* (xLineHighlightFormatFn)(const char* s, void* arg);

/// Experimental: Convenience function for highlighting with bbcodes.
/// Can be called in a `xLineHighlightFn` callback to colorize the `input` using the 
/// the provided `formatted` input that is the styled `input` with bbcodes. The 
/// content of `formatted` without bbcode tags should match `input` exactly.
void xLineHighlightFormatted(xLineHighlightEnv* henv, const char* input, const char* formatted);

/// \}

//--------------------------------------------------------------
// Readline with a specific completer and highlighter
//--------------------------------------------------------------

/// \defgroup readline
/// \{

/// Read input from the user using rich editing abilities, 
/// using a particular completion function and highlighter for this call only.
/// both can be NULL in which case the defaults are used.
/// @see xLineReadline(), xLineSetPromptMarker(), xLineSetDefaultCompleter(), xLineSetDefaultHighlighter().
char* xLineReadlineEx(const char* prompt_text, xLineCompleterFn* completer, void* completer_arg,
                                              xLineHighlightFn* highlighter, void* highlighter_arg);

/// \}


//--------------------------------------------------------------
// Options
//--------------------------------------------------------------

/// \defgroup options Options
/// \{

/// Set a prompt marker and a potential marker for extra lines with multiline input. 
/// Pass \a NULL for the `prompt_marker` for the default marker (`"> "`).
/// Pass \a NULL for continuation prompt marker to make it equal to the `prompt_marker`.
void xLineSetPromptMarker( const char* prompt_marker, const char* continuation_prompt_marker );

/// Get the current prompt marker.
const char* xLineGetPromptMarker(void);

/// Get the current continuation prompt marker.
const char* xLineGetContinuationPromptMarker(void);

/// Disable or enable multi-line input (enabled by default).
/// Returns the previous setting.
bool xLineEnableMultiline( bool enable );

/// Disable or enable sound (enabled by default).
/// A beep is used when tab cannot find any completion for example.
/// Returns the previous setting.
bool xLineEnableBeep( bool enable );

/// Disable or enable color output (enabled by default).
/// Returns the previous setting.
bool xLineEnableColor( bool enable );

/// Disable or enable duplicate entries in the history (disabled by default).
/// Returns the previous setting.
bool xLineEnableHistoryDuplicates( bool enable );

/// Disable or enable automatic tab completion after a completion 
/// to expand as far as possible if the completions are unique. (disabled by default).
/// Returns the previous setting.
bool xLineEnableAutoTab( bool enable );

/// Disable or enable preview of a completion selection (enabled by default)
/// Returns the previous setting.
bool xLineEnableCompletionPreview( bool enable );

/// Disable or enable automatic identation of continuation lines in multiline
/// input so it aligns with the initial prompt.
/// Returns the previous setting.
bool xLineEnableMultilineIndent(bool enable);

/// Disable or enable display of short help messages for history search etc.
/// (full help is always dispayed when pressing F1 regardless of this setting)
/// @returns the previous setting.
bool xLineEnableInlineHelp(bool enable);

/// Disable or enable hinting (enabled by default)
/// Shows a hint inline when there is a single possible completion.
/// @returns the previous setting.
bool xLineEnableHint(bool enable);

/// Set millisecond delay before a hint is displayed. Can be zero. (500ms by default).
long xLineSetHintDelay(long delay_ms);

/// Disable or enable syntax highlighting (enabled by default).
/// This applies regardless whether a syntax highlighter callback was set (`ic_set_highlighter`)
/// Returns the previous setting.
bool xLineEnableHighlight(bool enable);


/// Set millisecond delay for reading escape sequences in order to distinguish
/// a lone ESC from the start of a escape sequence. The defaults are 100ms and 10ms, 
/// but it may be increased if working with very slow terminals.
void xLineSetTtyEscDelay(long initial_delay_ms, long followup_delay_ms);

/// Enable highlighting of matching braces (and error highlight unmatched braces).`
bool xLineEnableBraceMatching(bool enable);

/// Set matching brace pairs.
/// Pass \a NULL for the default `"()[]{}"`.
void xLineSetMatchingBraces(const char* brace_pairs);

/// Enable automatic brace insertion (enabled by default).
bool xLineEnableBraceInsertion(bool enable);

/// Set matching brace pairs for automatic insertion.
/// Pass \a NULL for the default `()[]{}\"\"''`
void xLineSetInsertionBraces(const char* brace_pairs);

/// \}


//--------------------------------------------------------------
// Advanced Completion
//--------------------------------------------------------------

/// \defgroup completex Advanced Completion
/// \{

/// Get the raw current input (and cursor position if `cursor` != NULL) for the completion.
/// Usually completer functions should look at their `prefix` though as transformers
/// like `xLineCompleteWord` may modify the prefix (for example, unescape it).
const char* xLineCompletionInput( xLineCompletionEnv* cenv, long* cursor );

/// Get the completion argument passed to `ic_set_completer`.
void* xLineCompletionArg( const xLineCompletionEnv* cenv );

/// Do we have already some completions?
bool xLineHasCompletions( const xLineCompletionEnv* cenv );

/// Do we already have enough completions and should we return if possible? (for improved latency)
bool xLineStopCompleting( const xLineCompletionEnv* cenv);


/// Primitive completion, cannot be used with most transformers (like `xLineCompleteWord` and `xLineCompleteQword`).
/// When completed, `delete_before` _bytes_ are deleted before the cursor position,
/// `delete_after` _bytes_ are deleted after the cursor, and finally `completion` is inserted.
/// The `display` is used to display the completion in the completion menu, and `help` is displayed
/// with hinting. Both `display` and `help` can be NULL.
/// (all are copied by isocline and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible completions.
/// If `false` is returned, the callback should try to return and not add more completions (for improved latency).
bool xLineAddCompletionPrim( xLineCompletionEnv* cenv, const char* completion, 
                              const char* display, const char* help, 
                               long delete_before, long delete_after);

/// \}

//--------------------------------------------------------------
/// \defgroup helper Character Classes.
/// Convenience functions for character classes, highlighting and completion.
/// \{

/// Convenience: return the position of a previous code point in a UTF-8 string `s` from postion `pos`.
/// Returns `-1` if `pos <= 0` or `pos > strlen(s)` (or other errors).
long xLinePrevChar( const char* s, long pos );

/// Convenience: return the position of the next code point in a UTF-8 string `s` from postion `pos`.
/// Returns `-1` if `pos < 0` or `pos >= strlen(s)` (or other errors).
long xLineNextChar( const char* s, long pos );

/// Convenience: does a string `s` starts with a given `prefix` ?
bool xLineStartsWith( const char* s, const char* prefix );

/// Convenience: does a string `s` starts with a given `prefix` ignoring (ascii) case?
bool xLineIstartsWith( const char* s, const char* prefix );


/// Convenience: character class for whitespace `[ \t\r\n]`.
bool xLineCharIsWhite(const char* s, long len);

/// Convenience: character class for non-whitespace `[^ \t\r\n]`.
bool xLineCharIsNonwhite(const char* s, long len);

/// Convenience: character class for separators.
/// (``[ \t\r\n,.;:/\\(){}\[\]]``.)
/// This is used for word boundaries in isocline.
bool xLineCharIsSeparator(const char* s, long len);

/// Convenience: character class for non-separators.
bool xLineCharIsNonseparator(const char* s, long len);

/// Convenience: character class for letters (`[A-Za-z]` and any unicode > 0x80).
bool xLineCharIsLetter(const char* s, long len);

/// Convenience: character class for digits (`[0-9]`).
bool xLineCharIsDigit(const char* s, long len);

/// Convenience: character class for hexadecimal digits (`[A-Fa-f0-9]`).
bool xLineCharIsHexdigit(const char* s, long len);

/// Convenience: character class for identifier letters (`[A-Za-z0-9_-]` and any unicode > 0x80).
bool xLineCharIsIdletter(const char* s, long len);

/// Convenience: character class for filename letters (_not in_ " \t\r\n`@$><=;|&\{\}\(\)\[\]]").
bool xLineCharIsFilenameLetter(const char* s, long len);


/// Convenience: If this is a token start, return the length. Otherwise return 0.
long xLineIsToken(const char* s, long pos, xLineIsCharClassFn* is_token_char);

/// Convenience: Does this match the specified token? 
/// Ensures not to match prefixes or suffixes, and returns the length of the match (in bytes).
/// E.g. `xLineMatchToken("function",0,&xLineCharIsLetter,"fun")` returns 0.
/// while `xLineMatchToken("fun x",0,&xLineCharIsLetter,"fun"})` returns 3.
long xLineMatchToken(const char* s, long pos, xLineIsCharClassFn* is_token_char, const char* token);


/// Convenience: Do any of the specified tokens match? 
/// Ensures not to match prefixes or suffixes, and returns the length of the match (in bytes).
/// E.g. `xLineMatchAnyToken("function",0,&xLineCharIsLetter,{"fun","func",NULL})` returns 0.
/// while `xLineMatchAnyToken("func x",0,&xLineCharIsLetter,{"fun","func",NULL})` returns 4.
long xLineMatchAnyToken(const char* s, long pos, xLineIsCharClassFn* is_token_char, const char** tokens);

/// \}

//--------------------------------------------------------------
/// \defgroup term Terminal
///
/// Experimental: Low level terminal output.
/// Ensures basic ANSI SGR escape sequences are processed 
/// in a portable way (e.g. on Windows)
/// \{

/// Initialize for terminal output.
/// Call this before using the terminal write functions (`xLineTermWrite`)
/// Does nothing on most platforms but on Windows it sets the console to UTF8 output and possible 
/// enables virtual terminal processing.
void xLineTermInit(void);

/// Call this when done with the terminal functions.
void xLineTermDone(void);

/// Flush the terminal output. 
/// (happens automatically on newline characters ('\n') as well).
void xLineTermFlush(void);

/// Write a string to the console (and process CSI escape sequences).
void xLineTermWrite(const char* s);

/// Write a string to the console and end with a newline 
/// (and process CSI escape sequences).
void xLineTermWriteln(const char* s);

/// Write a formatted string to the console.
/// (and process CSI escape sequences)
void xLineTermWritef(const char* fmt, ...);

/// Write a formatted string to the console.
void xLineTermVwritef(const char* fmt, va_list args);

/// Set text attributes from a style.
void xLineTermStyle( const char* style );

/// Set text attribute to bold.
void xLineTermBold(bool enable);

/// Set text attribute to underline.
void xLineTermUnderline(bool enable);

/// Set text attribute to italic.
void xLineTermItalic(bool enable);

/// Set text attribute to reverse video.
void xLineTermReverse(bool enable);

/// Set text attribute to ansi color palette index between 0 and 255 (or 256 for the ANSI "default" color).
/// (auto matched to smaller palette if not supported)
void xLineTermColorAnsi(bool foreground, int color);

/// Set text attribute to 24-bit RGB color (between `0x000000` and `0xFFFFFF`).
/// (auto matched to smaller palette if not supported)
void xLineTermColorRgb(bool foreground, uint32_t color );

/// Reset the text attributes.
void xLineTermReset( void );

/// Get the palette used by the terminal:
/// This is usually initialized from the COLORTERM environment variable. The 
/// possible values of COLORTERM for each palette are given in parenthesis.
///
/// - 1: monochrome (`monochrome`)
/// - 3: old ANSI terminal with 8 colors, using bold for bright (`8color`/`3bit`)
/// - 4: regular ANSI terminal with 16 colors.     (`16color`/`4bit`)
/// - 8: terminal with ANSI 256 color palette.     (`256color`/`8bit`)
/// - 24: true-color terminal with full RGB colors. (`truecolor`/`24bit`/`direct`)
int xLineTermGetColorBits( void );

/// \}

//--------------------------------------------------------------
/// \defgroup async ASync
/// Async support
/// \{

/// Thread-safe way to asynchronously unblock a readline.
/// Behaves as if the user pressed the `ctrl-C` character
/// (resulting in returning NULL from `xLineReadline`).
/// Returns `true` if the event was successfully delivered.
/// (This may not be supported on all platforms, but it is
/// functional on Linux, macOS and Windows).
bool xLineAsyncStop(void);


//--------------------------------------------------------------
// FD-level async line editing.
//
// These primitives let a caller drive line editing from its own
// event loop (poll/select/kqueue). While a session is pending the
// caller may print chunks above the current edit line using
// xLinePrintAbove() without corrupting the user's input.
//
// Call sequence:
//
//   xLineHandle* h = xLineBegin("> ");
//   int fd = xLineFd(h);
//   // register `fd` for read-readiness in your event loop...
//   for (;;) {
//     poll/select/... on fd (and on your other sources);
//     xLineStepResult r = xLineStep(h);
//     if (r == XLINE_STEP_LINE) { char* s = xLineTake(h); ...; xLineFree(s); break; }
//     if (r == XLINE_STEP_EOF || r == XLINE_STEP_ERROR) break;
//     // r == XLINE_STEP_PENDING: keep looping
//   }
//   xLineEnd(h);
//
// The synchronous xLineReadline() is internally implemented in
// terms of these primitives; the two modes MUST NOT be used at
// the same time — calling xLineReadline() while a handle is live
// returns NULL and logs a warning.
//--------------------------------------------------------------

/// Opaque async line-editing session handle.
typedef struct xLineHandle_s xLineHandle;

/// Result of one non-blocking step.
typedef enum xLineStepResult_e {
  XLINE_STEP_PENDING = 0,  ///< more input needed; poll the fd again
  XLINE_STEP_LINE    = 1,  ///< a line is ready; call xLineTake()
  XLINE_STEP_EOF     = 2,  ///< Ctrl-D on empty input or stream closed
  XLINE_STEP_ERROR   = 3,  ///< unrecoverable error
} xLineStepResult;

/// Start a new async line-editing session.
/// Takes ownership of no memory; `prompt_text` may be NULL for "".
/// Returns NULL if another session is already live, or if the
/// terminal is not capable of interactive editing (pipe/dumb tty);
/// in the latter case callers should fall back to xLineReadline().
xLineHandle* xLineBegin(const char* prompt_text);

/// Return the fd the session is listening on.
///
/// POSIX: returns the tty/stdin fd (always >= 0). The fd is owned
/// by the library; the caller MUST NOT close it or change its
/// blocking mode.
///
/// Windows: returns -1. The console input handle is not an fd and
/// cannot be polled with select/WSAPoll; Windows callers should
/// block on xLineStep() in a worker thread or use a future
/// xLineStepBlocking() helper (not implemented in this PR).
int xLineFd(xLineHandle* h);

/// Process whatever input is currently ready on the session fd.
/// Non-blocking: returns immediately if no bytes are available.
/// May be called any number of times per poll wake-up.
xLineStepResult xLineStep(xLineHandle* h);

/// After xLineStep() returned XLINE_STEP_LINE, take ownership of
/// the edited line. The returned string is heap-allocated and
/// must be freed by xLineFree(). Returns NULL if no line is ready
/// (caller violated the protocol).
char* xLineTake(xLineHandle* h);

/// End a session. Safe to call in any state (PENDING to cancel,
/// after LINE once the line has been taken, after EOF/ERROR).
/// Restores the terminal to cooked mode and frees the handle.
void xLineEnd(xLineHandle* h);

/// Print `s` above the current edit line, preserving the prompt
/// and the user's in-progress input. Safe to call from the same
/// thread that owns the handle between xLineStep() invocations.
/// `s` is a plain string; embed "\n" for multi-line output. Does
/// nothing if `h` is NULL or the session is not live.
void xLinePrintAbove(xLineHandle* h, const char* s);

/// \}

//--------------------------------------------------------------
/// \defgroup alloc Custom Allocation
/// Register allocation functions for custom allocators
/// \{

typedef void* (xLineMallocFn)( size_t size );
typedef void* (xLineReallocFn)( void* p, size_t newsize );
typedef void  (xLineFreeFn)( void* p );

// NOTE: upstream's ic_init_custom_alloc() has been removed in xline.
// The library always uses the stdlib allocator (malloc/realloc/free).
// The three typedefs above are kept for source-compat with downstream
// code that still names them, but there is no public way to inject a
// custom allocator anymore.

/// Free a potentially custom alloc'd pointer (in particular, the result returned from `xLineReadline`)
void xLineFree( void* p );

/// Allocate using the current memory allocator.
void* xLineMalloc(size_t sz);

/// Duplicate a string using the current memory allocator.
const char* xLineStrdup( const char* s );

/// \}

#ifdef __cplusplus
}
#endif

#endif /// XLINE_LINE_H
