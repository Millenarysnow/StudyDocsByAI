// ============================================================================
// MetaIR/ClassMeta.cs — 一个完整的 C++ 可绑定类的元数据
//
// 这是整个系统的核心中间表示 (IR):
//   - 和输入格式无关 (XML / C++ 宏 / JSON 都产出这个)
//   - 和输出格式无关 (Lua / Python / 编辑器 都消费这个)
//
// 设计原则:
//   - 字段名语义清晰, 不带输入/输出格式的残留
//   - 用 record class 获得值相等语义 + 不可变性
// ============================================================================
using System.Collections.Generic;
using System.Text;

namespace MetaBindingGen.MetaIR;

public sealed class ClassMeta
{
    public required string CppClassName  { get; init; }   // C++ 类名
    public required string LuaClassName  { get; init; }   // Lua 侧暴露的名字
    public required string HeaderInclude { get; init; }   // 需要 #include 的头文件路径

    public List<MethodMeta> Methods { get; init; } = new();
    public List<MemberMeta> Members { get; init; } = new();

    public override string ToString()
    {
        var sb = new StringBuilder();
        sb.AppendLine($"ClassMeta(CppClassName={CppClassName},");
        sb.AppendLine($"          LuaClassName={LuaClassName},");
        sb.AppendLine($"          HeaderInclude={HeaderInclude},");
        sb.AppendLine($"          Methods=[");
        foreach (var m in Methods)
            sb.AppendLine($"            {m},");
        sb.AppendLine($"          ],");
        sb.AppendLine($"          Members=[");
        foreach (var m in Members)
            sb.AppendLine($"            {m},");
        sb.Append("          ])");
        return sb.ToString();
    }
}
