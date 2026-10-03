{"fallback-S25b", 3, 3, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("S", true);
        if (bar == 2) { host.strategy_entry("B", true); host.strategy_exit("XB", "B", 105.0, 95.0); }
        if (bar == 7) host.strategy_close_all(); },
    {
        {"B", "XB", true, 1, 100, 95, -5, 3, 3},
        {"S", "__close__", true, 1, 100, 100, 0, 1, 8}
    }},
