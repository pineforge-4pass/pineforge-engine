{"fallback-S43", 3, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) { host.strategy_order("O", false, 2.0); host.strategy_entry("A", true, na, na, 1.0); host.strategy_entry("B", false, na, na, 1.0); }
        if (bar == 7) host.strategy_close_all(); },
    {
        {"O", "A", false, 1, 100, 100, 0, 1, 1},
        {"O", "__close__", false, 1, 100, 100, 0, 1, 8},
        {"B", "__close__", false, 1, 100, 100, 0, 1, 8}
    }},
