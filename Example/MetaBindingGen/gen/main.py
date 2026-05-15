"""
main.py — 命令行入口

用法:
    python main.py --frontend xml --input ../sample_xml --output ../generated_xml
    python main.py --frontend cpp --input ../sample_cpp --output ../generated_cpp
"""

import argparse
import sys
from pathlib import Path

# 让同目录下的其他模块能 import
sys.path.insert(0, str(Path(__file__).parent))

import frontend_xml
import frontend_cpp
import backend_lua


def main():
    parser = argparse.ArgumentParser(description="C++/Lua 绑定代码生成器")
    parser.add_argument(
        "--frontend",
        choices=["xml", "cpp"],
        required=True,
        help="元数据来源: xml (独立 XML 文件) 或 cpp (源码内宏标注)",
    )
    parser.add_argument(
        "--input",
        required=True,
        help="输入目录 (包含 .meta 或带标注的 .h 文件)",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="输出目录 (生成的 .h 文件放这里)",
    )
    args = parser.parse_args()

    input_dir = Path(args.input)
    output_dir = Path(args.output)

    if not input_dir.is_dir():
        print(f"ERROR: 输入目录不存在: {input_dir}")
        sys.exit(1)

    # === 前端: 根据选择加载不同的解析器 ===
    print(f"=== Using frontend: {args.frontend} ===")
    if args.frontend == "xml":
        metas = frontend_xml.scan_directory(input_dir)
    else:
        metas = frontend_cpp.scan_directory(input_dir)

    if not metas:
        print("No classes found!")
        sys.exit(1)

    # === 中间结果: 打印 IR ===
    print(f"\n=== Intermediate Representation (IR) ===")
    for meta in metas:
        print(meta)
        print()

    # === 后端: 生成代码 (和前端无关) ===
    print(f"=== Generating code into {output_dir} ===")
    source_desc = f"frontend={args.frontend}, input={input_dir}"
    for meta in metas:
        out_file = backend_lua.write_to_file(meta, output_dir, source_desc)
        print(f"  wrote: {out_file}")

    print(f"\nDone! {len(metas)} class(es) processed.")


if __name__ == "__main__":
    main()
