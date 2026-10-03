{"fallback-Y3", 10, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("seed", true);
        if (bar == 2) {
            host.strategy_entry("S", false);
            host.strategy_entry("L", true);
            host.strategy_entry("S2", false);
            host.strategy_close("seed");
        }
        if (bar == 7) host.strategy_close_all();
    },
    {
        {"seed", "S", true, 1, 100, 100, 0, 1, 3},
        {"S", "L", false, 1, 100, 100, 0, 3, 3},
        {"L", "S2", true, 1, 100, 100, 0, 3, 3},
        {"S2", "__close__", false, 1, 100, 100, 0, 3, 8}
    }},
