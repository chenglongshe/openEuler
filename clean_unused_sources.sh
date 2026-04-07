#!/bin/bash
# 功能：剔除内核源码中未参与编译的 .c/.h/.rs 文件（基于 inotify 监控）
# 用法：./clean_unused_sources.sh <内核源码目录> <输出未使用文件列表> [编译命令]
#
# 原脚本存在以下问题，已在本版本中修复：
# 1. 路径格式不一致：find 和 inotifywait 输出的路径格式可能不同，导致 comm 比较失败
#    修复：使用 realpath 将源码目录统一为绝对路径
# 2. inotifywait --format '%w%f' 路径拼接问题：%w 末尾带 '/'，可能产生双斜杠
#    修复：对访问列表中的路径进行规范化（去除多余斜杠）
# 3. --exclude 正则表达式有误：'^.*/\.' 在 inotifywait 中匹配不正确
#    修复：改用更精确的排除模式
# 4. 访问列表未过滤文件类型：inotifywait 记录了所有类型的文件访问，
#    与只含 .c/.h/.rs 的 ALL_FILES 做 comm 比较时需要先过滤
#    修复：在比较前过滤访问列表，只保留 .c/.h/.rs 文件
# 5. 临时文件 ${ACCESSED_LIST}.sorted 未清理
#    修复：加入 trap 清理
# 6. inotify watch 数量限制可能不足（内核源码文件众多）
#    修复：添加检查和提示
# 7. include/ 目录必须被监控，原脚本的 --exclude 模式不会排除 include/，
#    但本版本明确确认了这一点

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "用法: $0 <内核源码目录> <输出未使用文件列表> [编译命令]"
    echo "示例: $0 /root/FusionOS_compile_env ./unused.txt 'make -j4'"
    exit 1
fi

SRC_DIR="$1"
OUTPUT_FILE="$2"
shift 2
MAKE_CMD="${*:-make}"

# 检查 inotifywait 是否可用
if ! command -v inotifywait &>/dev/null; then
    echo "错误：未找到 inotifywait，请先安装 inotify-tools：yum install inotify-tools"
    exit 1
fi

# 检查源码目录
if [ ! -d "$SRC_DIR" ]; then
    echo "错误：目录 $SRC_DIR 不存在"
    exit 1
fi

# 将源码目录转换为绝对路径，避免 find 和 inotifywait 输出路径格式不一致
SRC_DIR="$(realpath "$SRC_DIR")"

# 检查 inotify watch 数量限制
CURRENT_MAX_WATCHES=$(cat /proc/sys/fs/inotify/max_user_watches 2>/dev/null || echo 0)
RECOMMENDED_WATCHES=524288
if [ "$CURRENT_MAX_WATCHES" -lt "$RECOMMENDED_WATCHES" ]; then
    echo "警告：当前 inotify watch 上限为 $CURRENT_MAX_WATCHES，内核源码可能需要更多。"
    echo "建议执行：echo $RECOMMENDED_WATCHES | sudo tee /proc/sys/fs/inotify/max_user_watches"
    echo "尝试自动增加..."
    if echo "$RECOMMENDED_WATCHES" > /proc/sys/fs/inotify/max_user_watches 2>/dev/null; then
        echo "已将 max_user_watches 增加到 $RECOMMENDED_WATCHES"
    else
        echo "警告：无法自动增加，请手动执行上述命令（需要 root 权限）"
    fi
fi

# 临时文件
ACCESSED_LIST=$(mktemp)
ACCESSED_SORTED=$(mktemp)
ALL_FILES=$(mktemp)
cleanup() {
    rm -f "$ACCESSED_LIST" "$ACCESSED_SORTED" "$ALL_FILES"
}
trap cleanup EXIT

# 启动 inotify 监控（递归监控 open 事件）
# 注意：
#   - 排除 .git 和 Documentation 目录以降低监控开销
#   - 不排除 include/、scripts/、tools/ 等目录，因为编译过程可能访问其中的头文件
#   - --format '%w%f' 输出完整路径（%w=被监控目录路径，%f=文件名）
echo "正在启动文件访问监控..."
inotifywait -m -r -e open \
    --exclude '(/\.git/|/Documentation/)' \
    --format '%w%f' \
    "$SRC_DIR" > "$ACCESSED_LIST" 2>/dev/null &
INOTIFY_PID=$!

# 等待监控进程就绪（检查进程是否启动成功）
sleep 2
if ! kill -0 "$INOTIFY_PID" 2>/dev/null; then
    echo "错误：inotifywait 启动失败，请检查 inotify watch 限制"
    exit 1
fi
echo "文件访问监控已就绪 (PID: $INOTIFY_PID)"

# 执行编译
echo "正在执行编译命令: $MAKE_CMD"
ORIG_DIR="$(pwd)"
cd "$SRC_DIR"
eval "$MAKE_CMD"
BUILD_STATUS=$?
cd "$ORIG_DIR"

if [ "$BUILD_STATUS" -ne 0 ]; then
    echo "警告：编译命令返回非零状态码: $BUILD_STATUS"
fi

# 等待一小段时间，确保 inotify 刷新完缓冲区中的事件
sleep 1

# 停止监控
kill "$INOTIFY_PID" 2>/dev/null || true
wait "$INOTIFY_PID" 2>/dev/null || true
echo "文件访问监控已停止"

# 收集所有 .c/.h/.rs 文件（绝对路径，排序）
echo "正在收集源码文件列表..."
find "$SRC_DIR" -type f \( -name "*.c" -o -name "*.h" -o -name "*.rs" \) | sort > "$ALL_FILES"
TOTAL_COUNT=$(wc -l < "$ALL_FILES")
echo "共发现 $TOTAL_COUNT 个 .c/.h/.rs 源码文件"

# 处理访问列表：
#   1. 规范化路径（去除多余斜杠，如 //）
#   2. 只保留 .c/.h/.rs 文件
#   3. 去重并排序
echo "正在分析访问记录..."
sed 's|/\+|/|g' "$ACCESSED_LIST" | grep -E '\.(c|h|rs)$' | sort -u > "$ACCESSED_SORTED"
ACCESSED_COUNT=$(wc -l < "$ACCESSED_SORTED")
echo "编译过程中共访问 $ACCESSED_COUNT 个 .c/.h/.rs 文件"

# 找出未在访问列表中的文件（在 ALL_FILES 中但不在 ACCESSED_SORTED 中）
UNUSED_FILE_LIST=$(comm -23 "$ALL_FILES" "$ACCESSED_SORTED")

# 输出结果
if [ -n "$UNUSED_FILE_LIST" ]; then
    echo "$UNUSED_FILE_LIST" > "$OUTPUT_FILE"
    UNUSED_COUNT=$(echo "$UNUSED_FILE_LIST" | wc -l)

    # 统计 include 目录下未使用的 .h 文件数量
    INCLUDE_UNUSED=$(echo "$UNUSED_FILE_LIST" | grep -c "^${SRC_DIR}/include/" || true)

    echo ""
    echo "===== 分析结果 ====="
    echo "总源码文件数:          $TOTAL_COUNT"
    echo "参与编译的文件数:      $ACCESSED_COUNT"
    echo "未参与编译的文件数:    $UNUSED_COUNT"
    echo "其中 include/ 下未使用的 .h 文件: $INCLUDE_UNUSED"
    echo "未使用文件列表已保存到: $OUTPUT_FILE"
    echo ""

    read -p "是否删除这些文件？(y/N) " -r
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "$UNUSED_FILE_LIST" | xargs rm -f
        echo "已删除 $UNUSED_COUNT 个未使用的文件。"
    else
        echo "未删除任何文件。"
    fi
else
    echo "未发现未参与编译的 .c/.h/.rs 文件。"
fi
