import re, os, sys

d = "/Users/leo/Workspace/mivinci/xkit/modules/xline"

# Pattern for commented-out code (to be deleted)
CODE_PAT = re.compile(
    r"^\s*//\s*(?:if|for|while|return|else|break|continue|switch|case|goto|do)\b"
    r"|^\s*//\s*\}"
    r"|^\s*//\s*\{"
    r"|^\s*//\s*#"
    r"|^\s*//\s*\w+\s*[\(=;]"
)

PREPROC_PAT = re.compile(r"^\s*#\s*(?:define|include|if|ifdef|ifndef|else|elif|endif|pragma)\b")

# URL pattern (contains //)
URL_PAT = re.compile(r'https?://|<https?://')

total_changed = 0
total_deleted = 0


for fname in sorted(os.listdir(d)):
    if not (fname.endswith(".c") or fname.endswith(".h")):
        continue
    if fname == "LICENSE.isocline":
        continue
    fp = os.path.join(d, fname)
    text = open(fp).read()
    lines = text.split("\n")
    result = []
    i = 0
    changed = False
    deleted = 0

    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip()
        rstripped = line.rstrip()

        # Case 1: Preprocessor line with trailing // comment
        # e.g. #define _XOPEN_SOURCE 700  // so wcwidth is visible
        # Keep // as-is on preprocessor lines
        if PREPROC_PAT.match(line) and "//" in stripped and not stripped.startswith("//"):
            result.append(line)
            i += 1
            continue

        # Case 2: #endif with trailing // comment
        # e.g. #endif /// XLINE_LINE_H -> #endif /* XLINE_LINE_H */
        if re.match(r"^\s*#\s*endif\b", line) and "//" in stripped:
            # Replace // with /* ... */
            idx = line.rfind("//")
            if idx >= 0:
                before = line[:idx].rstrip()
                comment = line[idx + 2:].strip()
                if comment == "":
                    result.append(before + "  /* */")
                else:
                    result.append(before + "  /* " + comment + " */")
                changed = True
                i += 1
                continue

        # Case 3: Standalone // comment (starts line with //)
        if stripped.startswith("//"):
            # Check if this is commented-out code
            if CODE_PAT.match(line):
                # Skip (delete) the line
                deleted += 1
                changed = True
                i += 1
                continue

            # Check for ///< (Doxygen trailing comment for previous declaration)
            # ///< should become /**< ... */
            if stripped.startswith("///<"):
                content = stripped[4:].strip()
                if content == "":
                    result.append("/**< */")
                else:
                    result.append("/**< " + content + " */")
                changed = True
                i += 1
                continue

            # Collect consecutive // lines into a single /* ... */ block
            doc_lines = []
            while i < len(lines) and lines[i].lstrip().startswith("//"):
                s = lines[i].lstrip()
                # Remove // prefix, keeping one space if present
                content = s[2:]
                if content.startswith(" "):
                    content = content[1:]  # remove leading space after //
                doc_lines.append(content.rstrip())
                i += 1

            if len(doc_lines) == 1:
                if doc_lines[0] == "":
                    result.append("/* */")
                else:
                    result.append("/* " + doc_lines[0] + " */")
            else:
                result.append("/* " + doc_lines[0])
                for j in range(1, len(doc_lines)):
                    if doc_lines[j] == "":
                        result.append(" *")
                    else:
                        result.append(" * " + doc_lines[j])
                result.append(" */")
            changed = True
            continue

        # Case 4: Trailing ///< comment on a code line
        # e.g. XLINE_STEP_PENDING = 0, ///< more input needed
        # Should become: XLINE_STEP_PENDING = 0, /**< more input needed */
        if "///<" in stripped and not stripped.startswith("//"):
            idx = line.find("///<")
            if idx >= 0:
                before = line[:idx].rstrip()
                comment = line[idx + 4:].strip()
                if comment == "":
                    result.append(before + "  /**< */")
                else:
                    result.append(before + "  /**< " + comment + " */")
                changed = True
                i += 1
                continue

        # Case 5: Trailing // comment on a code line
        # e.g. return KEY_F5; // minicom
        # Should become: return KEY_F5; /* minicom */
        # But must avoid lines containing URLs (https://...)
        if "//" in stripped and not stripped.startswith("//"):
            # Check if the line contains a URL
            if URL_PAT.search(line):
                result.append(line)
                i += 1
                continue

            # Find the // that's NOT at the start of the line
            # We need to be careful not to confuse // in URLs or strings
            idx = line.rfind("//")
            if idx > 0:
                before = line[:idx].rstrip()
                after = line[idx + 2:].strip()
                if after == "":
                    result.append(before + "  /* */")
                else:
                    result.append(before + "  /* " + after + " */")
                changed = True
                i += 1
                continue

        # Default: keep line as-is
        result.append(line)
        i += 1

    if changed:
        # Remove consecutive blank lines (may appear after deleting code)
        cleaned = []
        prev_blank = False
        for line in result:
            is_blank = line.strip() == ""
            if is_blank and prev_blank:
                continue
            cleaned.append(line)
            prev_blank = is_blank

        # Ensure file ends with newline
        text_out = "\n".join(cleaned)
        if not text_out.endswith("\n"):
            text_out += "\n"

        open(fp, "w").write(text_out)
        total_deleted += deleted
        total_changed += 1
        print(f"FIXED: {fname} (deleted {deleted} commented-out code lines)")
    else:
        print(f"OK: {fname}")

print(f"\nTotal: {total_changed} files changed, {total_deleted} commented-out code lines removed")
