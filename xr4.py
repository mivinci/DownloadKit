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
    changed = False
    deleted = 0

    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)//\s?(.*)", line)
        if not m:
            result.append(line)
            continue

        indent = m.group(1)
        content = m.group(2).rstrip()

        # Check if this is commented-out code
        if CODE_PAT.match(line):
            # Skip (delete) the line
            deleted += 1
            changed = True
            continue

        # Check if this is a trailing comment (code before //)
        # e.g. "  x = 5;  // set x"
        # We handle this by looking for non-whitespace before //
        stripped = line.lstrip()
        if len(stripped) < len(line) - len(indent):
            # There's content before // - this is a trailing comment
            # Find the // that's not at the start
            idx = line.find("//", len(indent))
            if idx > len(indent):
                before = line[:idx].rstrip()
                comment = line[idx + 2:].strip()
                result.append(before + "  /* " + comment + " */")
                changed = True
                continue

        # Standalone // comment -> /* comment */
        if content == "":
            result.append(indent + "/* */")
        else:
            result.append(indent + "/* " + content + " */")
        changed = True

    if changed:
        # Remove blank lines that might appear after deletion
        # (consecutive blank lines -> single blank line)
        cleaned = []
        prev_blank = False
        for line in result:
            is_blank = line.strip() == ""
            if is_blank and prev_blank:
                continue
            cleaned.append(line)
            prev_blank = is_blank

        open(fp, "w").write("\n".join(cleaned))
        total_deleted += deleted
        total_changed += 1
        print(f"FIXED: {fname} (deleted {deleted} commented-out code lines)")
    else:
        print(f"OK: {fname}")

print(f"\nTotal: {total_changed} files changed, {total_deleted} commented-out code lines removed")
