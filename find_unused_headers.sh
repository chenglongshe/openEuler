#!/bin/bash
# ============================================================================
# find_unused_headers.sh
# 识别 include/ 目录下不参与内核编译的 .h 文件
#
# 原理:
#   1. 扫描内核源码中所有 #include 指令，提取被引用的头文件路径
#   2. 收集 include/ 下所有 .h 文件
#   3. 对比找出从未被任何源码引用的头文件
#
# 已处理的特殊情况:
#   - uapi/ 头文件同时可通过 <uapi/linux/foo.h> 和 <linux/foo.h> 引用
#   - asm-generic/Kbuild 中 mandatory-y 列出的头文件（架构回退机制）
#   - Makefile 中通过 -include 强制包含的头文件
#   - 相对路径 #include "xxx" 引用（如 trace/stages/）
#   - 排除 include/generated/ 和 include/config/ (编译时生成)
#   - 同时检查 .dts/.dtsi 设备树文件中 dt-bindings 的引用
#   - 检查 Kbuild/Makefile 中的 header-y 导出声明
# ============================================================================

set -e

KERNEL_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$KERNEL_DIR"

# 输出文件
OUTPUT_FILE="unused_headers.txt"
SUMMARY_FILE="unused_headers_summary.txt"

echo "=================================================================="
echo " 内核 include/ 目录未使用头文件检测工具"
echo "=================================================================="
echo ""
echo "内核源码目录: $KERNEL_DIR"
echo "开始分析..."
echo ""

# ------------------------------------------------------------------
# 第一步：提取所有源码中的 #include 引用路径
# ------------------------------------------------------------------
echo "[1/5] 扫描源码中的 #include 引用..."

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# 从所有源码文件中提取 #include <xxx> 和 #include "xxx" 路径
# 搜索范围: .c .h .S .dts .dtsi 文件
find . -path ./.git -prune -o \
    \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.dts' -o -name '*.dtsi' \) \
    -print0 | \
    xargs -0 grep -h '^\s*#\s*include\s*[<"]' 2>/dev/null | \
    sed -n 's/.*#\s*include\s*[<"]\([^>"]*\)[>"].*/\1/p' | \
    sort -u > "$TMPDIR/all_includes.txt"

INCLUDE_COUNT=$(wc -l < "$TMPDIR/all_includes.txt")
echo "   找到 $INCLUDE_COUNT 个不同的 #include 引用路径"

# ------------------------------------------------------------------
# 第二步：提取 Makefile 中 -include 强制包含的头文件
# ------------------------------------------------------------------
echo "[2/5] 扫描 Makefile 中的 -include 强制引用..."

# 提取 Makefile 中 -include $(srctree)/include/xxx 的路径
find . -path ./.git -prune -o \
    \( -name 'Makefile' -o -name 'Makefile.*' -o -name '*.mk' \) \
    -print0 | \
    xargs -0 grep -h '\-include.*\$(srctree)/include/' 2>/dev/null | \
    sed -n 's/.*-include[[:space:]]*\$(srctree)\/include\/\([^[:space:]\\]*\).*/\1/p' | \
    sort -u > "$TMPDIR/force_includes.txt"

# 提取 Makefile 中生成 #include 语句的代码（如 modpost.c 生成 #include <linux/xxx>）
find . -path ./.git -prune -o \
    \( -name '*.c' -o -name '*.py' -o -name '*.sh' \) \
    -print0 | \
    xargs -0 grep -h 'include.*<linux/[^>]*>' 2>/dev/null | \
    sed -n 's/.*<linux\/\([^>"]*\)>.*/linux\/\1/p' | \
    sort -u >> "$TMPDIR/force_includes.txt"

FORCE_COUNT=$(wc -l < "$TMPDIR/force_includes.txt")
echo "   找到 $FORCE_COUNT 个 Makefile/-include 强制引用"

# ------------------------------------------------------------------
# 第三步：提取 Kbuild mandatory-y 头文件列表
# ------------------------------------------------------------------
echo "[3/5] 扫描 Kbuild mandatory-y / header-y 声明..."

# asm-generic/Kbuild 和 uapi Kbuild 中的 mandatory-y 和 header-y
find include/ -name 'Kbuild' -print0 | \
    xargs -0 grep -h '^\s*\(mandatory-y\|header-y\|genhdr-y\)\s*+=' 2>/dev/null | \
    sed 's/.*+=\s*//' | tr -d ' ' | \
    sort -u > "$TMPDIR/kbuild_headers.txt"

KBUILD_COUNT=$(wc -l < "$TMPDIR/kbuild_headers.txt")
echo "   找到 $KBUILD_COUNT 个 Kbuild mandatory-y/header-y 声明"

# ------------------------------------------------------------------
# 第四步：收集 include/ 下所有 .h 文件并对比
# ------------------------------------------------------------------
echo "[4/5] 收集并对比 include/ 目录下的 .h 文件..."

# 排除 generated/ 和 config/ 目录（编译时生成）
find include/ -name '*.h' \
    -not -path 'include/generated/*' \
    -not -path 'include/config/*' | \
    sort > "$TMPDIR/all_headers.txt"

HEADER_COUNT=$(wc -l < "$TMPDIR/all_headers.txt")
echo "   找到 $HEADER_COUNT 个头文件"

# 对比分析
> "$TMPDIR/unused_headers.txt"

while IFS= read -r header_path; do
    # 去掉 "include/" 前缀，得到 #include 中的路径
    inc_path="${header_path#include/}"
    basename_h=$(basename "$header_path")

    found=0

    # ---- 检查方式1: 直接路径匹配 (最常见) ----
    # 例如: include/linux/kernel.h -> 搜索 "linux/kernel.h"
    if grep -qFx "$inc_path" "$TMPDIR/all_includes.txt" 2>/dev/null; then
        found=1
    fi

    # ---- 检查方式2: uapi 头文件的短路径 ----
    # include/uapi/linux/foo.h -> 搜索 "linux/foo.h"
    if [ $found -eq 0 ] && [[ "$inc_path" == uapi/* ]]; then
        short_path="${inc_path#uapi/}"
        if grep -qFx "$short_path" "$TMPDIR/all_includes.txt" 2>/dev/null; then
            found=1
        fi
    fi

    # ---- 检查方式3: 相对路径 #include "xxx" ----
    # 例如 trace/stages/stage1_struct_define.h 可能被同目录的文件
    # 通过 #include "stages/stage1_struct_define.h" 引用
    if [ $found -eq 0 ]; then
        # 尝试匹配路径的后缀部分
        # 如 trace/stages/stage1.h 可匹配 "stages/stage1.h"
        if grep -qF "$basename_h" "$TMPDIR/all_includes.txt" 2>/dev/null; then
            # 进一步确认：在 includes 列表中查找以此文件名结尾的路径
            while IFS= read -r candidate; do
                # 检查候选路径是否是头文件路径的后缀
                if [[ "$inc_path" == *"$candidate" ]]; then
                    found=1
                    break
                fi
            done < <(grep -F "$basename_h" "$TMPDIR/all_includes.txt")
        fi
    fi

    # ---- 检查方式4: Makefile -include 强制包含 ----
    if [ $found -eq 0 ]; then
        if grep -qFx "$inc_path" "$TMPDIR/force_includes.txt" 2>/dev/null; then
            found=1
        fi
    fi

    # ---- 检查方式5: asm-generic Kbuild mandatory-y ----
    # include/asm-generic/xxx.h -> Kbuild 声明 mandatory-y += xxx.h
    if [ $found -eq 0 ] && [[ "$inc_path" == asm-generic/* ]]; then
        if grep -qFx "$basename_h" "$TMPDIR/kbuild_headers.txt" 2>/dev/null; then
            found=1
        fi
    fi

    # ---- 检查方式6: uapi asm-generic Kbuild ----
    if [ $found -eq 0 ] && [[ "$inc_path" == uapi/asm-generic/* ]]; then
        if grep -qFx "$basename_h" "$TMPDIR/kbuild_headers.txt" 2>/dev/null; then
            found=1
        fi
    fi

    if [ $found -eq 0 ]; then
        echo "$header_path" >> "$TMPDIR/unused_headers.txt"
    fi
done < "$TMPDIR/all_headers.txt"

UNUSED_COUNT=$(wc -l < "$TMPDIR/unused_headers.txt")
echo "   发现 $UNUSED_COUNT 个未被引用的头文件"

# ------------------------------------------------------------------
# 第五步：生成报告
# ------------------------------------------------------------------
echo "[5/5] 生成报告..."

cp "$TMPDIR/unused_headers.txt" "$OUTPUT_FILE"

{
    echo "=================================================================="
    echo " 内核 include/ 目录未使用头文件检测报告"
    echo "=================================================================="
    echo ""
    echo "扫描时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "内核源码: $KERNEL_DIR"
    echo ""
    echo "统计信息:"
    echo "  头文件总数:         $HEADER_COUNT"
    echo "  被引用的头文件:     $((HEADER_COUNT - UNUSED_COUNT))"
    echo "  未引用的头文件:     $UNUSED_COUNT"
    echo "  未引用比例:         $(awk "BEGIN{printf \"%.1f%%\", $UNUSED_COUNT/$HEADER_COUNT*100}")"
    echo ""
    echo "检测方法:"
    echo "  1. #include <path> / #include \"path\" 直接引用"
    echo "  2. uapi/ 头文件短路径引用 (去除 uapi/ 前缀)"
    echo "  3. 相对路径引用 (#include \"relative/path.h\")"
    echo "  4. Makefile -include 强制包含"
    echo "  5. Kbuild mandatory-y / header-y 声明"
    echo ""
    echo "------------------------------------------------------------------"
    echo " 按目录分类统计"
    echo "------------------------------------------------------------------"

    awk -F/ '{
        if (NF >= 3) dir=$1"/"$2"/"
        else dir=$1"/"
        print dir
    }' "$OUTPUT_FILE" | sort | uniq -c | sort -rn | while read count dir; do
        printf "  %-45s %d 个\n" "$dir" "$count"
    done

    echo ""
    echo "------------------------------------------------------------------"
    echo " 未引用头文件完整列表"
    echo "------------------------------------------------------------------"
    cat "$OUTPUT_FILE"

    echo ""
    echo "------------------------------------------------------------------"
    echo " 注意事项"
    echo "------------------------------------------------------------------"
    echo "  以下类型的头文件即使未被源码直接引用也可能参与编译:"
    echo "  1. 被条件编译宏控制 (#ifdef CONFIG_xxx ... #include)"
    echo "  2. 通过 Kbuild header-y 导出到用户空间"
    echo "  3. 仅在特定架构或配置下使用"
    echo "  4. 被编译脚本动态生成的代码引用"
    echo "  建议进一步人工确认后再决定是否移除。"

} > "$SUMMARY_FILE"

echo ""
echo "=================================================================="
echo " 分析完成！"
echo "=================================================================="
echo ""
echo "  头文件总数:         $HEADER_COUNT"
echo "  被引用的头文件:     $((HEADER_COUNT - UNUSED_COUNT))"
echo "  未引用的头文件:     $UNUSED_COUNT"
echo "  未引用比例:         $(awk "BEGIN{printf \"%.1f%%\", $UNUSED_COUNT/$HEADER_COUNT*100}")"
echo ""
echo "输出文件:"
echo "  未引用文件列表: $OUTPUT_FILE"
echo "  详细分析报告:   $SUMMARY_FILE"

