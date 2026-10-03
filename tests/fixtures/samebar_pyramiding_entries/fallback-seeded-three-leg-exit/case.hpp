{"fallback-seeded-three-leg-exit", 7, 3, 0,
    [](FallbackHost& host, int bar) {
        if (bar == 0) host.strategy_entry("seed", false, na, na, 1.0);
        if (bar == 2) {
            host.strategy_entry("L", true, na, na, 1.0);
            host.strategy_entry("S", false, na, na, 1.0);
            host.strategy_entry("L2", true, na, na, 1.0);
            host.strategy_exit("X", "L2", 105.0, 95.0);
        }
        if (bar == 7) host.strategy_close_all();
    },
    {
        {"seed", "L", false, 1, 100, 100, 0, 1, 3},
        {"L", "S", true, 1, 100, 100, 0, 3, 3},
        {"L2", "X", true, 1, 100, 95, -5, 3, 3}
    }},
