{"fallback-S22b", 3, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("A", true, na, na, 2.0);
        if (bar == 1) host.strategy_entry("A2", true, na, na, 1.0);
        if (bar == 3) { host.strategy_order("O", true, 1.0); host.strategy_entry("M", true, na, na, 1.0); }
        if (bar == 7) host.strategy_close_all(); },
    {
        {"A", "__close__", true, 2, 100, 100, 0, 1, 8},
        {"A2", "__close__", true, 1, 100, 100, 0, 2, 8},
        {"O", "__close__", true, 1, 100, 100, 0, 4, 8}
    }},
