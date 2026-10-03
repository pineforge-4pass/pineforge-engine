{"fallback-X8c", 10, 3, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("seed", false);
        if (bar == 2) {
            host.strategy_entry("L", true);
            host.strategy_entry("S", false);
            host.strategy_entry("L2", true);
            host.strategy_exit("X", "L2", 105.0, 95.0);
        }
        if (bar == 7) host.strategy_close_all();
    },
    {
        {"seed", "L", false, 1, 100, 100, 0, 1, 3},
        {"L", "S", true, 1, 100, 100, 0, 3, 3},
        {"S", "L2", false, 1, 100, 100, 0, 3, 3},
        {"L2", "__close__", true, 1, 100, 100, 0, 3, 8}
    }},
