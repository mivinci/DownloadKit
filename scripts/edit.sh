#!/bin/sh

set -e

usage() {
    cat <<'EOF'
Usage: edit [options] --file <path>

Options:
  --file <path>       目标文件 (必填)
  --lines <range>     行号范围，如 "5", "5-8"
  --replace <text>    替换内容 (与 --delete 互斥)
  --delete            删除指定行 (与 --replace 互斥)
  --after <n>         在第 n 行后插入
  --before <n>        在第 n 行前插入
  --preview           只显示 diff，不实际修改
  --backup            自动备份原文件 (加 .bak)
  --help              显示此帮助

Examples:
  edit --file main.py --lines 10-15 --replace "new code"
  edit --file main.py --lines 20 --delete
  edit --file main.py --after 5 --replace "inserted line"
EOF
    exit 1
}

# 解析参数
FILE=""
RANGE=""
REPLACE=""
DELETE=false
PREVIEW=false
BACKUP=false
AFTER=""
BEFORE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --file) FILE="$2"; shift 2 ;;
        --lines) RANGE="$2"; shift 2 ;;
        --replace) REPLACE="$2"; shift 2 ;;
        --delete) DELETE=true; shift ;;
        --preview) PREVIEW=true; shift ;;
        --backup) BACKUP=true; shift ;;
        --after) AFTER="$2"; shift 2 ;;
        --before) BEFORE="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

[ -z "$FILE" ] && { echo "Error: --file is required"; usage; }
[ ! -f "$FILE" ] && { echo "Error: file not found: $FILE"; exit 1; }

TMP_FILE=$(mktemp)
trap 'rm -f "$TMP_FILE"' EXIT

# ---------- INSERT 模式 ----------
if [ -n "$AFTER" ] || [ -n "$BEFORE" ]; then
    [ -n "$RANGE" ] && { echo "Error: --lines conflicts with --after/--before"; exit 1; }
    [ "$DELETE" = true ] && { echo "Error: --delete not supported with --after/--before"; exit 1; }
    [ -z "$REPLACE" ] && { echo "Error: --replace is required"; exit 1; }

    INSERT_LINE=$((${AFTER:-$BEFORE} + 0))
    [ "$INSERT_LINE" -lt 1 ] && { echo "Error: line number must be >= 1"; exit 1; }
    INSERT_AFTER=$([ -n "$AFTER" ] && printf true || printf false)

    current=0
    inserted=false
    while IFS= read -r line || [ -n "$line" ]; do
        current=$((current + 1))
        if [ "$INSERT_AFTER" = false ] && [ "$current" -eq "$INSERT_LINE" ] && [ "$inserted" = false ]; then
            printf '%s\n' "$REPLACE" >> "$TMP_FILE"
            inserted=true
        fi
        printf '%s\n' "$line" >> "$TMP_FILE"
        if [ "$INSERT_AFTER" = true ] && [ "$current" -eq "$INSERT_LINE" ] && [ "$inserted" = false ]; then
            printf '%s\n' "$REPLACE" >> "$TMP_FILE"
            inserted=true
        fi
    done < "$FILE"
    if [ "$inserted" = false ]; then
        while [ "$current" -lt "$INSERT_LINE" ]; do
            current=$((current + 1))
            printf '\n' >> "$TMP_FILE"
        done
        printf '%s\n' "$REPLACE" >> "$TMP_FILE"
    fi

# ---------- DELETE 模式 ----------
elif [ "$DELETE" = true ]; then
    [ -z "$RANGE" ] && { echo "Error: --lines is required with --delete"; exit 1; }

    case "$RANGE" in
        *-*) START_LINE=$(echo "$RANGE" | cut -d- -f1); END_LINE=$(echo "$RANGE" | cut -d- -f2) ;;
        *) START_LINE="$RANGE"; END_LINE="$RANGE" ;;
    esac
    START_LINE=$((START_LINE + 0)); END_LINE=$((END_LINE + 0))
    [ "$START_LINE" -lt 1 ] && { echo "Error: line number must be >= 1"; exit 1; }

    current=0
    while IFS= read -r line || [ -n "$line" ]; do
        current=$((current + 1))
        if [ "$current" -lt "$START_LINE" ] || [ "$current" -gt "$END_LINE" ]; then
            printf '%s\n' "$line" >> "$TMP_FILE"
        fi
    done < "$FILE"

# ---------- REPLACE 模式 ----------
else
    [ -z "$RANGE" ] && { echo "Error: --lines is required"; exit 1; }
    [ -z "$REPLACE" ] && { echo "Error: --replace is required"; exit 1; }

    case "$RANGE" in
        *-*) START_LINE=$(echo "$RANGE" | cut -d- -f1); END_LINE=$(echo "$RANGE" | cut -d- -f2) ;;
        *) START_LINE="$RANGE"; END_LINE="$RANGE" ;;
    esac
    START_LINE=$((START_LINE + 0)); END_LINE=$((END_LINE + 0))
    [ "$START_LINE" -lt 1 ] && { echo "Error: line number must be >= 1"; exit 1; }
    [ "$END_LINE" -lt "$START_LINE" ] && { echo "Error: end line < start line"; exit 1; }

    current=0
    replaced=false
    while IFS= read -r line || [ -n "$line" ]; do
        current=$((current + 1))
        if [ "$current" -lt "$START_LINE" ]; then
            # 替换范围之前的行：原样写入
            printf '%s\n' "$line" >> "$TMP_FILE"
        elif [ "$current" -eq "$START_LINE" ]; then
            # 到达替换起点：写入替换内容
            printf '%s\n' "$REPLACE" >> "$TMP_FILE"
            replaced=true
            # 跳过要替换的行（从 START_LINE+1 到 END_LINE）
            skip=$((END_LINE - START_LINE))
            while [ $skip -gt 0 ]; do
                if IFS= read -r _; then
                    : # 跳过
                else
                    break
                fi
                skip=$((skip - 1))
                current=$((current + 1))
            done
        else
            # 替换范围之后的行：原样写入
            printf '%s\n' "$line" >> "$TMP_FILE"
        fi
    done < "$FILE"

    # 如果文件行数不足，补空行到目标行再写入
    if [ "$replaced" = false ]; then
        echo "Warning: file has only $current lines, target $START_LINE out of range" >&2
        while [ "$current" -lt "$START_LINE" ]; do
            current=$((current + 1))
            printf '\n' >> "$TMP_FILE"
        done
        printf '%s\n' "$REPLACE" >> "$TMP_FILE"
    fi
fi

# ---------- 输出 ----------
if [ "$PREVIEW" = true ]; then
    if diff -u "$FILE" "$TMP_FILE" > /dev/null 2>&1; then
        echo "(no changes)"
    else
        diff -u "$FILE" "$TMP_FILE" || true
    fi
    echo "--- Preview only, no changes made. ---"
    exit 0
fi

if [ "$BACKUP" = true ]; then
    cp "$FILE" "${FILE}.bak"
    echo "Backup saved: ${FILE}.bak"
fi

mv "$TMP_FILE" "$FILE"
trap - EXIT
echo "Done: $FILE modified."
