{"fallback-S42", 3, -1, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) { host.strategy_entry("A", true, na, na, 1.0); host.strategy_entry("P", false, 100.5, na, 1.0);
                      host.strategy_entry("B", false, na, na, 1.0); host.strategy_entry("C", true, na, na, 1.0); }
        if (bar == 7) { host.strategy_cancel_all(); host.strategy_close_all(); } },
    {
        {"A", "B", true, 1, 100, 100, 0, 1, 1},
        {"C", "P", true, 1, 100, 100.5, 0.5, 1, 1},
        {"P", "__close__", false, 1, 100.5, 100, 0.5, 1, 8}
    }},
