{"fallback-S25", 3, 3, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("S", false);
        if (bar == 2) { host.strategy_entry("B", false); host.strategy_exit("XB", "B", 95.0, 105.0); }
        if (bar == 7) host.strategy_close_all(); },
    {
        {"B", "XB", false, 1, 100, 95, 5, 3, 3},
        {"S", "__close__", false, 1, 100, 100, 0, 1, 8}
    }},
