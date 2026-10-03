{"fallback-S2", 2, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("A", true, na, na, 1.0);
        if (bar == 1) host.strategy_entry("S", true, na, 150.0, 1.0);
        if (bar == 2) host.strategy_entry("M", true, na, na, 1.0);
        if (bar == 7) { host.strategy_cancel_all(); host.strategy_close_all(); } },
    {
        {"A", "__close__", true, 1, 100, 100, 0, 1, 8}
    }},
