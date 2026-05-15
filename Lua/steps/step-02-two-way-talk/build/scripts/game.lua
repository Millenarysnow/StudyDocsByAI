-- scripts/game.lua
-- Step 02: Lua 和 C++ 双向交互

print("[Lua]  game.lua loaded")

-- ============================================================
-- 1. Lua 调 C 函数 (C++ 注册了 add 和 c_greet)
-- ============================================================

function test_c_calls()
    print("[Lua]  About to call C function add(10, 20)")

    -- add 是 C++ 里 my_add, 被注册为全局 add
    local sum = add(10, 20)

    print("[Lua]  Result from C: " .. sum)
    print("")

    print("[Lua]  Greeting C")
    c_greet("Greetings, C++!")
    print("[Lua]  (c_greet returned nothing)")
end

-- ============================================================
-- 2. 定义一个函数给 C++ 调
-- ============================================================

function to_upper(s)
    print("[Lua]  Lua function called with: " .. s)
    return string.upper(s)
end
