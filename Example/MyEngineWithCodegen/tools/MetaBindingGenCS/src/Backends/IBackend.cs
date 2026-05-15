// ============================================================================
// Backends/IBackend.cs — 后端抽象接口
//
// 后端消费 ClassMeta, 生成目标代码。加新后端 = 实现这个接口。
// ============================================================================
using System.Collections.Generic;
using MetaBindingGen.MetaIR;

namespace MetaBindingGen.Backends;

public interface IBackend
{
    /// <summary>
    /// 后端的唯一标识, 比如 "lua" / "python" / "editor"。
    /// </summary>
    string Name { get; }

    /// <summary>
    /// 为所有类生成代码并写入 outputDir。
    /// </summary>
    /// <param name="metas">IR</param>
    /// <param name="outputDir">输出目录</param>
    /// <param name="sourceDesc">来源描述 (用于写入生成代码的注释)</param>
    void Generate(IReadOnlyList<ClassMeta> metas, string outputDir, string sourceDesc);
}
