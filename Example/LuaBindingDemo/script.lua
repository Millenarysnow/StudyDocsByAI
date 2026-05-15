-- script.lua — 演示 Lua 端掌控主流程 + 调用 C++ 类

App = {}
App.__index = App

function App:new()
    local self = setmetatable({}, App)
    self.tick_count = 0
    return self
end

function App:initialize()
    print("[Lua] App:initialize() called")

    -- 创建一个 C++ MyObject 对象 (走到 C++ 的 class_new<MyObject>)
    self.obj = MyObject:new("Sample")

    -- 调用 C++ 的 setValue
    self.obj:setValue(100)

    print("[Lua] Initial value = " .. self.obj:getValue())
    print("[Lua] Object name = " .. self.obj:getName())
end

function App:tick(dt)
    self.tick_count = self.tick_count + 1

    -- 每 60 帧打印一次状态,调用 C++ 方法
    if self.tick_count % 60 == 0 then
        local v = self.obj:getValue()
        print(string.format(
            "[Lua] frame=%d  value=%d  dt=%.4f",
            self.tick_count, v, dt))

        -- 演示调 C++ 成员函数
        self.obj:add(-10)

        if self.obj:getValue() <= 0 then
            print("[Lua] Value reached zero! Resetting...")
            self.obj:setValue(100)
        end
    end
end

function App:clear()
    print("[Lua] App:clear() called")
    self.obj = nil
end
