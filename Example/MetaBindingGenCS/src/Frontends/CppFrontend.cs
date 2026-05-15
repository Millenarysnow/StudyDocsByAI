// ============================================================================
// Frontends/CppFrontend.cs — 方案 B 前端: 扫 C++ 源码里的 BIND_LUA_* 宏标注
//
// 对应"元数据内嵌源码"的风格 (类似 Unreal UFUNCTION/UPROPERTY 宏)。
// 程序员只维护 .h 文件, 标注要导出的成员。
//
// 约定的宏:
//   BIND_LUA_CLASS()   — 放在 class 前
//   BIND_LUA_METHOD()  — 放在方法前
//   BIND_LUA_MEMBER()  — 放在成员变量前
//
// 示例:
//   BIND_LUA_CLASS()
//   class MyWidget {
//       BIND_LUA_METHOD()
//       void setText(const char* s);
//       BIND_LUA_MEMBER()
//       int m_width;
//   };
//
// 实现说明:
//   本实现用正则表达式扫源码 (简化版)。
//   生产环境推荐用 Roslyn 扫 C# 元数据, 或 libclang 扫 C++ (更严谨, 能处理
//   模板/宏/多重继承等边界情况)。
// ============================================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using MetaBindingGen.MetaIR;

namespace MetaBindingGen.Frontends;

public sealed class CppFrontend : IFrontend
{
    public string Name => "cpp";

    // --- 正则表达式 ---

    // 类声明: "BIND_LUA_CLASS()\nclass ClassName"
    private static readonly Regex ClassRegex = new(
        @"BIND_LUA_CLASS\s*\(\s*\)\s*\r?\n\s*class\s+(?<name>\w+)",
        RegexOptions.Compiled | RegexOptions.Multiline);

    // 方法: "BIND_LUA_METHOD()\n<返回类型> methodName(...)  [const]"
    private static readonly Regex MethodRegex = new(
        @"BIND_LUA_METHOD\s*\(\s*\)\s*\r?\n" +                 // 标注
        @"\s*(?<returnType>[\w:\s<>\*&,]+?)\s+" +              // 返回类型
        @"(?<name>\w+)\s*" +                                   // 方法名
        @"\([^)]*\)" +                                         // 参数列表
        @"\s*(?<constness>const)?",                            // 可能的 const
        RegexOptions.Compiled | RegexOptions.Multiline);

    // 成员: "BIND_LUA_MEMBER()\n<类型> memberName;"
    private static readonly Regex MemberRegex = new(
        @"BIND_LUA_MEMBER\s*\(\s*\)\s*\r?\n" +                 // 标注
        @"\s*(?<type>[\w:\s<>\*&]+?)\s+" +                     // 类型
        @"(?<name>\w+)\s*;",                                   // 成员名
        RegexOptions.Compiled | RegexOptions.Multiline);

    public IReadOnlyList<ClassMeta> ScanDirectory(string inputDir)
    {
        var results = new List<ClassMeta>();
        foreach (var file in Directory.EnumerateFiles(inputDir, "*.h", SearchOption.AllDirectories))
        {
            var content = File.ReadAllText(file);
            if (!content.Contains("BIND_LUA_CLASS"))
                continue;  // 跳过没标注的头文件

            try
            {
                var meta = ParseHeaderFile(file, content);
                results.Add(meta);
                Console.WriteLine($"[cpp-frontend] parsed: {Path.GetFileName(file)} -> {meta.CppClassName}");
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[cpp-frontend] ERROR in {file}: {e.Message}");
            }
        }
        return results;
    }

    private static ClassMeta ParseHeaderFile(string path, string content)
    {
        // 1. 找类名
        var classMatch = ClassRegex.Match(content);
        if (!classMatch.Success)
            throw new InvalidDataException("未找到 BIND_LUA_CLASS() 标注");

        var cppName = classMatch.Groups["name"].Value;

        // 从类声明位置开始扫后面的内容, 避免扫到其他类
        var body = content.Substring(classMatch.Index + classMatch.Length);

        var result = new ClassMeta
        {
            CppClassName  = cppName,
            LuaClassName  = cppName,
            HeaderInclude = Path.GetFileName(path),
        };

        // 2. 扫方法
        foreach (Match m in MethodRegex.Matches(body))
        {
            var name = m.Groups["name"].Value;
            var isConst = m.Groups["constness"].Success && m.Groups["constness"].Value == "const";
            result.Methods.Add(new MethodMeta(
                LuaName: name,
                CppName: name,
                IsConst: isConst,
                IsStatic: false));
        }

        // 3. 扫成员
        foreach (Match m in MemberRegex.Matches(body))
        {
            var name = m.Groups["name"].Value;
            var type = m.Groups["type"].Value.Trim();

            // 约定: C++ 里叫 m_xxx, Lua 里去掉 m_ 前缀
            var luaName = name.StartsWith("m_") ? name.Substring(2) : name;

            result.Members.Add(new MemberMeta(
                LuaName: luaName,
                CppName: name,
                TypeName: type));
        }

        return result;
    }
}
