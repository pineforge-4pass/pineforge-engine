{"live-exit-two-sided-entry", 3, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("A", true, na, na, 1.0);
        if (bar == 1) host.strategy_exit("X", "A", 150.0, 50.0);
        if (bar == 2) {
            host.strategy_entry("S", false, na, na, 1.0);
            host.strategy_entry("L", true, na, na, 1.0);
        }
        if (bar == 7) {
            host.strategy_close_all();
        }
    },
    {
        {"A", "S", true, 1, 100, 100, 0, 1, 3},
        {"L", "S", true, 1, 100, 100, 0, 3, 3},
        {"L", "__close__", true, 1, 100, 100, 0, 3, 8}
    }},
