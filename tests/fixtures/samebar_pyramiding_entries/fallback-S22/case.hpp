{"fallback-S22", 3, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) { host.strategy_order("O", true, 1.0); host.strategy_entry("A", false, na, na, 1.0); host.strategy_entry("B", true, na, na, 1.0); }
        if (bar == 7) host.strategy_close_all(); },
    {
        {"O", "A", true, 1, 100, 100, 0, 1, 1},
        {"B", "__close__", true, 1, 100, 100, 0, 1, 8}
    }},
