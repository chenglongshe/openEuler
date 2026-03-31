#!/usr/bin/env python3
"""
apply_defconfig_patch.py — 将补丁文件中的修改同步到 fusionos_defconfig

用法:
    python3 apply_defconfig_patch.py [PATCH_FILE] [DEFCONFIG_FILE]

默认:
    PATCH_FILE   = 0001-disable-548-kernel-modules-in-fusionos_defconfig.patch
    DEFCONFIG_FILE = arch/x86/configs/fusionos_defconfig

功能:
    1. 解析 patch 文件，提取所有变更块（block-level）
    2. 在 defconfig 中按块替换：
       - 将匹配的旧行块替换为对应的新行块（支持 1:N 替换）
       - 纯新增行追加到文件末尾（跳过已存在的）
    3. 脚本幂等：多次执行结果一致
"""

import sys
import os


def parse_patch(patch_path):
    """解析 unified diff 补丁，提取变更块。

    每个变更块是 (old_lines, new_lines) 元组:
      - old_lines: 连续的 '-' 行内容（不含前缀）
      - new_lines: 紧随的 '+' 行内容（不含前缀）

    纯新增块的 old_lines 为空。

    返回:
        blocks: list of (old_lines, new_lines)
    """
    blocks = []

    with open(patch_path, "r") as f:
        lines = f.readlines()

    in_diff = False
    cur_old = []
    cur_new = []

    def flush():
        if cur_old or cur_new:
            blocks.append((list(cur_old), list(cur_new)))
        cur_old.clear()
        cur_new.clear()

    for line in lines:
        if line.startswith("diff --git"):
            in_diff = True
            flush()
            continue
        if not in_diff:
            continue
        if line.startswith("index ") or line.startswith("--- ") or line.startswith("+++ "):
            continue
        if line.startswith("@@"):
            flush()
            continue

        if line.startswith("-"):
            # 如果前面有 + 行但现在又看到 - 行，说明是新的变更块
            if cur_new and not cur_old:
                flush()
            cur_old.append(line[1:].rstrip("\n"))
        elif line.startswith("+"):
            cur_new.append(line[1:].rstrip("\n"))
        else:
            # 上下文行：结束当前变更块
            flush()

    flush()
    return blocks


def apply_changes(defconfig_path, blocks):
    """将变更块应用到 defconfig 文件。

    对于每个 (old_lines, new_lines) 块:
      - 如果 old_lines 非空：在 defconfig 中查找该连续行序列并替换为 new_lines
      - 如果 old_lines 为空：将 new_lines 中不存在于 defconfig 的行追加到末尾
    """
    with open(defconfig_path, "r") as f:
        content_lines = [l.rstrip("\n") for l in f.readlines()]

    replaced_count = 0
    removed_count = 0
    pending_adds = []  # 纯新增行

    for old_lines, new_lines in blocks:
        if not old_lines:
            # 纯新增
            pending_adds.extend(new_lines)
            continue

        # 查找 old_lines 块在 content_lines 中的位置
        found = False
        old_len = len(old_lines)
        for i in range(len(content_lines) - old_len + 1):
            if content_lines[i : i + old_len] == old_lines:
                # 替换
                content_lines[i : i + old_len] = new_lines
                replaced_count += old_len
                found = True
                break

        if not found:
            # 可能已经应用过：检查 new_lines 是否已存在
            new_len = len(new_lines)
            already_applied = False
            for i in range(len(content_lines) - new_len + 1):
                if content_lines[i : i + new_len] == new_lines:
                    already_applied = True
                    break
            if not already_applied and new_lines:
                # old_lines 未找到且 new_lines 也不存在，追加到待添加列表
                pending_adds.extend(new_lines)

    # 处理纯新增行（跳过已存在的 CONFIG 行，保留空行和注释）
    existing_set = set(content_lines)
    # 检查 "# Disabled drivers" 标记是否已存在
    marker_exists = "# Disabled drivers" in existing_set
    add_lines = []
    for line in pending_adds:
        if line == "" or (line.startswith("#") and "CONFIG_" not in line):
            # 空行和非 CONFIG 注释行：如果标记已存在则跳过（已应用过）
            if not marker_exists:
                add_lines.append(line)
        elif line not in existing_set:
            add_lines.append(line)

    added_count = len(add_lines)

    if add_lines:
        # 查找 "# Disabled drivers" 标记（可能已由上面的块替换添加）
        marker_idx = None
        for idx, line in enumerate(content_lines):
            if line.strip() == "# Disabled drivers":
                marker_idx = idx
                break

        if marker_idx is not None:
            insert_pos = marker_idx + 1
            for al in add_lines:
                content_lines.insert(insert_pos, al)
                insert_pos += 1
        else:
            # 如果待添加行已包含标记，不再额外添加
            has_marker = any(l.strip() == "# Disabled drivers" for l in add_lines)
            if not has_marker:
                content_lines.append("")
                content_lines.append("# Disabled drivers")
            content_lines.extend(add_lines)

    with open(defconfig_path, "w") as f:
        for line in content_lines:
            f.write(line + "\n")

    return replaced_count, removed_count, added_count


def main():
    # 获取脚本所在目录作为项目根目录
    script_dir = os.path.dirname(os.path.abspath(__file__))

    patch_file = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        script_dir, "0001-disable-548-kernel-modules-in-fusionos_defconfig.patch"
    )
    defconfig_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        script_dir, "arch/x86/configs/fusionos_defconfig"
    )

    if not os.path.isfile(patch_file):
        print(f"错误: 补丁文件不存在: {patch_file}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(defconfig_file):
        print(f"错误: defconfig 文件不存在: {defconfig_file}", file=sys.stderr)
        sys.exit(1)

    print(f"补丁文件: {patch_file}")
    print(f"目标文件: {defconfig_file}")
    print()

    blocks = parse_patch(patch_file)
    replace_blocks = sum(1 for old, new in blocks if old)
    add_blocks = sum(1 for old, new in blocks if not old)
    print(f"解析补丁完成:")
    print(f"  替换块: {replace_blocks} 个")
    print(f"  新增块: {add_blocks} 个")
    print()

    replaced, removed, added = apply_changes(defconfig_file, blocks)
    print(f"应用结果:")
    print(f"  替换: {replaced} 行")
    print(f"  删除: {removed} 行")
    print(f"  新增: {added} 行")

    if replaced == 0 and removed == 0 and added == 0:
        print("\n补丁已经应用过，无需更改。")
    else:
        print(f"\n已成功将补丁同步到 {defconfig_file}")


if __name__ == "__main__":
    main()
