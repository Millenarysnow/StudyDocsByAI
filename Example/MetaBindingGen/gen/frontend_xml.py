"""
frontend_xml.py — 方案 A 前端: XML 元数据解析器

对应"元数据和源码分离"风格: 元数据单独维护在 XML 文件里。

输入: *.meta 文件 (XML 格式) + 隐含的 *.h 文件
输出: ClassMeta 对象

示例 XML:
    <classDef name="MyWidget" header="my_widget.h">
        <method   lua_name="setText"  cpp_name="setText" />
        <method   lua_name="getText"  cpp_name="getText" const="true" />
        <member   lua_name="width"    cpp_name="m_width"  type="int" />
    </classDef>
"""

import xml.etree.ElementTree as ET
from pathlib import Path
from meta_ir import ClassMeta, MethodMeta, MemberMeta


def parse_meta_file(path: Path) -> ClassMeta:
    """解析一个 .meta XML 文件，返回 ClassMeta"""
    tree = ET.parse(path)
    root = tree.getroot()

    cpp_name = root.get("name")
    if not cpp_name:
        raise ValueError(f"{path}: classDef 缺少 name 属性")

    header = root.get("header", f"{cpp_name.lower()}.h")
    lua_name = root.get("lua_name", cpp_name)

    result = ClassMeta(
        cpp_class_name=cpp_name,
        lua_class_name=lua_name,
        header_include=header,
    )

    # 解析 <method> 节点
    for method_node in root.findall("method"):
        lua = method_node.get("lua_name")
        cpp = method_node.get("cpp_name")
        is_const = method_node.get("const", "false").lower() == "true"
        is_static = method_node.get("static", "false").lower() == "true"

        if not (lua and cpp):
            raise ValueError(f"{path}: method 缺少 lua_name 或 cpp_name")

        result.methods.append(MethodMeta(
            lua_name=lua, cpp_name=cpp,
            is_const=is_const, is_static=is_static,
        ))

    # 解析 <member> 节点
    for member_node in root.findall("member"):
        lua = member_node.get("lua_name")
        cpp = member_node.get("cpp_name")
        type_name = member_node.get("type")

        if not (lua and cpp and type_name):
            raise ValueError(f"{path}: member 缺少 lua_name/cpp_name/type")

        result.members.append(MemberMeta(
            lua_name=lua, cpp_name=cpp, type_name=type_name,
        ))

    return result


def scan_directory(dir_path: Path):
    """扫描一个目录下所有 .meta 文件，返回 ClassMeta 列表"""
    results = []
    for meta_file in dir_path.glob("*.meta"):
        try:
            meta = parse_meta_file(meta_file)
            results.append(meta)
            print(f"[xml-frontend] parsed: {meta_file.name} -> {meta.cpp_class_name}")
        except Exception as e:
            print(f"[xml-frontend] ERROR in {meta_file.name}: {e}")
    return results
