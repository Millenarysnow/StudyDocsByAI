-- scripts/game.lua
-- step-03 的脚本和 step-02 完全一致, 因为 C++ 侧只是重构没改行为

print("[Lua]  game.lua loaded")

function test_c_calls()
    print("[Lua]  About to call C function add(10, 20)")
    local sum = add(10, 20)
    print("[Lua]  Result from C: " .. sum)
    print("")

    print("[Lua]  Greeting C")
    c_greet("Greetings, C++!")
    print("[Lua]  (c_greet returned nothing)")
end

function to_upper(s)
    print("[Lua]  Lua function called with: " .. s)
    return string.upper(s)
end
