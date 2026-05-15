"""
frontend_cpp.py — 方案 B 前端: C++ 源码宏标注扫描器

对应 Unreal 风格: 元数据直接写在 C++ 源码里, 用宏标注要导出的方法/成员。

约定的标注宏:
    BIND_LUA_CLASS()     — 放在 class 定义前, 标记此类要导出
    BIND_LUA_METHOD()    — 放在方法前, 标记此方法要导出
    BIND_LUA_MEMBER()    — 放在成员变量前, 标记此成员要导出

示例 C++:
    BIND_LUA_CLASS()
    class MyWidget {
    public:
        BIND_LUA_METHOD()
        void setText(const char* s);
    
        BIND_LUA_METHOD()
        const char* getText() const;
    
        BIND_LUA_MEMBER()
        int m_width;
    };

本脚本用正则表达式扫源码 (简化实现)。
生产环境更严谨的做法是用 Clang libtooling, 能正确处理模板、宏、多重继承等。
"""

import re
from pathlib import Path
from meta_ir import ClassMeta, MethodMeta, MemberMeta


# ============ 正则表达式 ============
# 类声明: "BIND_LUA_CLASS()\nclass ClassName"
CLASS_PATTERN = re.compile(
    r'BIND_LUA_CLASS\s*\(\s*\)\s*\n\s*class\s+(\w+)',
    re.MULTILINE
)

# 方法: "BIND_LUA_METHOD()\n<返回类型> methodName(...)  [const]"
METHOD_PATTERN = re.compile(
    r'BIND_LUA_METHOD\s*\(\s*\)\s*\n'              # 标注
    r'\s*(?P<return_type>[\w:\s<>*&,]+?)\s+'       # 返回类型
    r'(?P<name>\w+)\s*'                            # 方法名
    r'\([^)]*\)'                                   # 参数列表 (忽略内容)
    r'\s*(?P<constness>const)?',                   # 可能的 const
    re.MULTILINE
)

# 成员: "BIND_LUA_MEMBER()\n<类型> memberName"
MEMBER_PATTERN = re.compile(
    r'BIND_LUA_MEMBER\s*\(\s*\)\s*\n'              # 标注
    r'\s*(?P<type>[\w:\s<>*&]+?)\s+'               # 类型
    r'(?P<name>\w+)\s*;',                          # 成员名
    re.MULTILINE
)


def parse_header_file(path: Path) -> ClassMeta:
    """解析一个带标注的 C++ 头文件，返回 ClassMeta"""
    content = path.read_text(encoding="utf-8")

    # 1. 找类名
    class_match = CLASS_PATTERN.search(content)
    if not class_match:
        raise ValueError(f"{path}: 未找到 BIND_LUA_CLASS() 标注")
    cpp_name = class_match.group(1)

    # 从类声明位置开始, 只扫后面的内容 (避免扫到其他类)
    class_body_start = class_match.end()
    body = content[class_body_start:]

    result = ClassMeta(
        cpp_class_name=cpp_name,
        lua_class_name=cpp_name,
        header_include=path.name,
    )

    # 2. 扫方法
    for m in METHOD_PATTERN.finditer(body):
        name = m.group("name")
        is_const = bool(m.group("constness"))
        result.methods.append(MethodMeta(
            lua_name=name,
            cpp_name=name,
            is_const=is_const,
            is_static=False,
        ))

    # 3. 扫成员
    for m in MEMBER_PATTERN.finditer(body):
        name = m.group("name")
        type_name = m.group("type").strip()
        # 约定: 代码里叫 m_xxx, Lua 里去掉 m_ 前缀
        lua_name = name[2:] if name.startswith("m_") else name
        result.members.append(MemberMeta(
            lua_name=lua_name,
            cpp_name=name,
            type_name=type_name,
        ))

    return result


def scan_directory(dir_path: Path):
    """扫描一个目录下所有 .h 文件, 返回 ClassMeta 列表"""
    results = []
    for header_file in dir_path.glob("*.h"):
        content = header_file.read_text(encoding="utf-8")
        if "BIND_LUA_CLASS" not in content:
            continue  # 跳过没标注的头
        try:
            meta = parse_header_file(header_file)
            results.append(meta)
            print(f"[cpp-frontend] parsed: {header_file.name} -> {meta.cpp_class_name}")
        except Exception as e:
            print(f"[cpp-frontend] ERROR in {header_file.name}: {e}")
    return results
