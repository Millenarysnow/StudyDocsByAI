-- scripts/game.lua — step-04

function to_upper(s)
    return string.upper(s)
end

function greet(name, age)
    print(string.format("[Lua]  greet: Hello %s, you are %d years old", name, age))
end

function fail()
    error("intentional error!")
end
