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

# Keep track of the previous non-blank line type
# to detect if we are inside a conditional compilation block

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
    
    # Track if we're inside a conditional compilation block (#if ... #endif)
    # that is surrounded by section dividers
    in_cond_block = False
    
    # Track if the current line is inside a /* */ block from section dividers
    in_block_comment = False

    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip()
        rstripped = line.rstrip()
        
        # -------------------------------------------------------------
        # If inside a block comment from a previous section divider,
        # just pass lines through until we find the closing */
        # -------------------------------------------------------------
        if in_block_comment:
            if "*/" in line:
                in_block_comment = False
                # Replace the closing */ with blank line
                result.append("")
                changed = True
            else:
                result.append(line)
            i += 1
            continue
        
        # -------------------------------------------------------------
        # Section dividers: //---... blocks
        # -------------------------------------------------------------
        if re.match(r"^\s*//-{10,}\s*$", line):
            # Look ahead to see what's between the //--- blocks
            title_lines = []
            j = i + 1
            is_doxygen = False
            has_code = False
            has_preproc = False
            while j < len(lines):
                if re.match(r"^\s*//-{10,}\s*$", lines[j]):
                    break
                s = lines[j].strip()
                if s.startswith("///"):
                    is_doxygen = True
                    break
                if s.startswith("//"):
                    t = s.lstrip("/ ").rstrip()
                    if t:
                        title_lines.append(t)
                    # Check if this is a preprocessor directive
                    if re.match(r"^\s*#\s*(?:define|include|if|ifdef|ifndef|else|elif|endif|pragma)\b", lines[j]):
                        has_preproc = True
                elif s == "":
                    pass
                else:
                    # Check if this is code (not comment)
                    # Could be a #if/#else/#endif block
                    if re.match(r"^\s*#\s*(?:if|ifdef|ifndef|else|elif|endif)\b", lines[j]):
                        has_preproc = True
                    else:
                        has_code = True
                    break
                j += 1
            
            if is_doxygen or has_code or j >= len(lines):
                # This is a group divider before /// doxygen or code
                # Replace with blank line
                result.append("")
                changed = True
                i += 1
                continue
            
            if has_preproc:
                # Conditional compilation block (#if/#else/#endif)
                # Replace the opening //--- with blank line
                result.append("")
                changed = True
                i += 1
                continue
            
            if title_lines:
                title = " ".join(title_lines).rstrip(".")
                result.append("/* ── " + title + " ── */")
                changed = True
                i = j + 1
                continue
            else:
                # Empty section between dashes, skip both
                changed = True
                i = j + 1
                continue
        
        # -------------------------------------------------------------
        # Standalone // comment lines
        # -------------------------------------------------------------
        if stripped.startswith("//"):
            # Check if this is commented-out code
            if CODE_PAT.match(line):
                # Skip (delete) the line
                deleted += 1
                changed = True
                i += 1
                continue
            
            # Check for ///< (Doxygen trailing comment for previous declaration)
            if stripped.startswith("///<"):
                content = stripped[4:].strip()
                if content == "":
                    result.append("/**< */")
                else:
                    result.append("/**< " + content + " */")
                changed = True
                i += 1
                continue
            
            # Check if inside a conditional compilation block
            # Preprocessor directives with // comments should keep //
            if PREPROC_PAT.match(line):
                result.append(line)
                i += 1
                continue
            
            # Check for #endif with trailing // comment
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
        
        # -------------------------------------------------------------
        # Lines with trailing ///< comment
        # -------------------------------------------------------------
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
        
        # -------------------------------------------------------------
        # Lines with trailing // comment (not URL, not ///<)
        # -------------------------------------------------------------
        if "//" in stripped and not stripped.startswith("//"):
            # Check if the line contains a URL - don't touch those
            if URL_PAT.search(line):
                result.append(line)
                i += 1
                continue
            
            # Check for preprocessor lines with trailing // comment
            # Keep // on preprocessor lines
            if PREPROC_PAT.match(line):
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
