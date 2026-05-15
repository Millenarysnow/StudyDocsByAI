-- scripts/hello.lua
-- 这是一个最基础的 Lua 脚本, 由 C++ 加载执行

print("Hello from Lua (file)")
print("Lua version: " .. _VERSION)

-- 做点简单计算, 验证 Lua 真的在工作
local a, b = 2, 3
print(string.format("%d + %d = %d", a, b, a + b))
