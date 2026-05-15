// ============================================================================
// Frontends/XmlFrontend.cs — 方案 A 前端: 独立 XML 元数据文件
//
// 对应"元数据和源码分离"的风格 (类似某些工业级引擎)。
// 程序员同时维护 my_widget.h 和 my_widget.meta 两个文件。
//
// 示例 XML:
//   <classDef name="MyWidget" header="my_widget.h">
//       <method lua_name="setText" cpp_name="setText" />
//       <method lua_name="getText" cpp_name="getText" const="true" />
//       <member lua_name="width"   cpp_name="m_width" type="int" />
//   </classDef>
// ============================================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Xml.Linq;
using MetaBindingGen.MetaIR;

namespace MetaBindingGen.Frontends;

public sealed class XmlFrontend : IFrontend
{
    public string Name => "xml";

    public IReadOnlyList<ClassMeta> ScanDirectory(string inputDir)
    {
        var results = new List<ClassMeta>();
        foreach (var file in Directory.EnumerateFiles(inputDir, "*.meta", SearchOption.AllDirectories))
        {
            try
            {
                var meta = ParseMetaFile(file);
                results.Add(meta);
                Console.WriteLine($"[xml-frontend] parsed: {Path.GetFileName(file)} -> {meta.CppClassName}");
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[xml-frontend] ERROR in {file}: {e.Message}");
            }
        }
        return results;
    }

    private static ClassMeta ParseMetaFile(string path)
    {
        var doc  = XDocument.Load(path);
        var root = doc.Root ?? throw new InvalidDataException($"{path}: XML 没有 root");

        if (root.Name.LocalName != "classDef")
            throw new InvalidDataException($"{path}: root 不是 <classDef>");

        var cppName       = GetRequiredAttr(root, "name");
        var headerInclude = (string?)root.Attribute("header") ?? $"{cppName.ToLower()}.h";
        var luaName       = (string?)root.Attribute("lua_name") ?? cppName;

        var result = new ClassMeta
        {
            CppClassName  = cppName,
            LuaClassName  = luaName,
            HeaderInclude = headerInclude,
        };

        // 解析所有 <method>
        foreach (var node in root.Elements("method"))
        {
            var lua = GetRequiredAttr(node, "lua_name");
            var cpp = GetRequiredAttr(node, "cpp_name");
            var isConst  = ((string?)node.Attribute("const"))?.Equals("true", StringComparison.OrdinalIgnoreCase) ?? false;
            var isStatic = ((string?)node.Attribute("static"))?.Equals("true", StringComparison.OrdinalIgnoreCase) ?? false;

            result.Methods.Add(new MethodMeta(lua, cpp, isConst, isStatic));
        }

        // 解析所有 <member>
        foreach (var node in root.Elements("member"))
        {
            var lua  = GetRequiredAttr(node, "lua_name");
            var cpp  = GetRequiredAttr(node, "cpp_name");
            var type = GetRequiredAttr(node, "type");

            result.Members.Add(new MemberMeta(lua, cpp, type));
        }

        return result;
    }

    private static string GetRequiredAttr(XElement node, string attrName)
    {
        var value = (string?)node.Attribute(attrName);
        if (string.IsNullOrEmpty(value))
            throw new InvalidDataException($"{node.Name.LocalName} 缺少 {attrName} 属性");
        return value;
    }
}
