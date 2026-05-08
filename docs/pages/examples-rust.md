# Example — Calling from Rust {#examples_rust}

@tableofcontents

A small, idiomatic Rust wrapper around the PineForge C ABI. Same shape
as the Python and C examples — load a strategy `.so`, push bars, read
the report.

## What you'll build

```
$ cargo run --release -- path/to/strategy.so path/to/ohlcv.csv
PineForge 0.1.1 (97c93d3) — 672 bars
49 trades, net pnl: -190.85
  L  pnl=  +12.40  qty=10.0
  S  pnl=  -22.10  qty=10.0
  ...
```

## `Cargo.toml`

```toml
[package]
name = "pineforge-rs-demo"
version = "0.1.0"
edition = "2021"

[dependencies]
libloading = "0.8"   # safe wrapper around dlopen / dlsym
```

## `src/main.rs`

```rust
use libloading::{Library, Symbol};
use std::env;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fs::File;
use std::io::{BufRead, BufReader};

// ── C ABI mirror ──────────────────────────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PfBar {
    open: f64, high: f64, low: f64, close: f64, volume: f64,
    timestamp: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PfTrade {
    entry_time: i64, exit_time: i64,
    entry_price: f64, exit_price: f64,
    pnl: f64, pnl_pct: f64,
    is_long: c_int,
    max_runup: f64, max_drawdown: f64,
    qty: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PfSecurityDiag {
    sec_id: c_int,
    feed_count: i64, complete_count: i64, partial_count: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PfTraceEntry {
    timestamp: i64, bar_index: i32, name_id: i32, value: f64,
}

#[repr(C)]
#[derive(Default)]
struct PfReport {
    total_trades: c_int,
    trades: *mut PfTrade,
    trades_len: c_int,
    net_profit: f64,

    input_bars_processed: i64,
    script_bars_processed: i64,

    security_feeds_total: i64,
    security_complete_total: i64,
    security_partial_total: i64,

    magnifier_sub_bars_total: i64,
    magnifier_sample_ticks_total: i64,

    input_tf_seconds: c_int,
    script_tf_seconds: c_int,
    script_tf_ratio: c_int,
    needs_aggregation: c_int,
    bar_magnifier_enabled: c_int,

    security_diag: *mut PfSecurityDiag,
    security_diag_len: c_int,

    trace: *mut PfTraceEntry,
    trace_len: c_int,
    trace_names: *mut *const c_char,
    trace_names_len: c_int,
}

const PF_MAGNIFIER_ENDPOINTS: c_int = 3;

// ── Safe wrapper ──────────────────────────────────────────────────────

struct StrategyLib {
    _lib: Library,   // own the lib so it outlives the symbols
    create:        unsafe extern "C" fn(*const c_char) -> *mut c_void,
    free_handle:   unsafe extern "C" fn(*mut c_void),
    run_full:      unsafe extern "C" fn(
        *mut c_void, *const PfBar, c_int,
        *const c_char, *const c_char,
        c_int, c_int, c_int,
        *mut PfReport,
    ),
    free_report:   unsafe extern "C" fn(*mut PfReport),
}

impl StrategyLib {
    fn open(path: &str) -> Result<Self, libloading::Error> {
        unsafe {
            let lib = Library::new(path)?;
            let create:      Symbol<unsafe extern "C" fn(*const c_char) -> *mut c_void>
                = lib.get(b"strategy_create")?;
            let free_handle: Symbol<unsafe extern "C" fn(*mut c_void)>
                = lib.get(b"strategy_free")?;
            let run_full: Symbol<unsafe extern "C" fn(
                *mut c_void, *const PfBar, c_int,
                *const c_char, *const c_char,
                c_int, c_int, c_int,
                *mut PfReport)>
                = lib.get(b"run_backtest_full")?;
            let free_report: Symbol<unsafe extern "C" fn(*mut PfReport)>
                = lib.get(b"report_free")?;
            Ok(Self {
                create:      *create,
                free_handle: *free_handle,
                run_full:    *run_full,
                free_report: *free_report,
                _lib: lib,
            })
        }
    }

    fn run(&self, bars: &[PfBar]) -> Vec<PfTrade> {
        unsafe {
            let h = (self.create)(std::ptr::null());
            let mut r = PfReport::default();
            (self.run_full)(h, bars.as_ptr(), bars.len() as c_int,
                            b"\0".as_ptr() as _, b"\0".as_ptr() as _,
                            0, 4, PF_MAGNIFIER_ENDPOINTS,
                            &mut r as *mut _);

            let trades: Vec<PfTrade> = std::slice::from_raw_parts(
                r.trades, r.trades_len as usize).to_vec();

            (self.free_report)(&mut r as *mut _);
            (self.free_handle)(h);
            trades
        }
    }
}

// ── Driver ────────────────────────────────────────────────────────────

fn load_csv(path: &str) -> std::io::Result<Vec<PfBar>> {
    let mut bars = Vec::new();
    let f = BufReader::new(File::open(path)?);
    for (i, line) in f.lines().enumerate() {
        if i == 0 { continue; }                              // header
        let line = line?;
        let mut cols = line.split(',');
        bars.push(PfBar {
            open:      cols.next().unwrap().parse().unwrap(),
            high:      cols.next().unwrap().parse().unwrap(),
            low:       cols.next().unwrap().parse().unwrap(),
            close:     cols.next().unwrap().parse().unwrap(),
            volume:    cols.next().unwrap().parse().unwrap(),
            timestamp: cols.next().unwrap().trim().parse().unwrap(),
        });
    }
    Ok(bars)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: {} STRATEGY.so OHLCV.csv", args[0]);
        std::process::exit(2);
    }
    let bars = load_csv(&args[2]).expect("csv");
    println!("loaded {} bars", bars.len());

    let lib = StrategyLib::open(&args[1]).expect("dlopen");
    let trades = lib.run(&bars);
    let net: f64 = trades.iter().map(|t| t.pnl).sum();

    println!("{} trades, net pnl: {:+.2}", trades.len(), net);
    for t in trades.iter().take(10) {
        println!("  {} pnl={:+8.2}  qty={:.1}",
                 if t.is_long != 0 { 'L' } else { 'S' },
                 t.pnl, t.qty);
    }
}
```

## Caveats

- **Lifetime of `trades`** — copied out before `report_free`, so the
  `Vec<PfTrade>` outlives the report.
- **`trace_names`** — if you enable tracing, copy the strings out
  before `strategy_free` (their backing memory belongs to the handle).
- **Threading** — `StrategyLib` is `Send` but **not** `Sync`. One
  handle per thread; many handles per process is fine.
- **Empty-string TFs** — Rust string literals are `&str`, not C strings.
  Use `b"\0".as_ptr()` or `CString::new("")?.into_raw()` and remember
  to reclaim it.

## See also

- [FFI from Python](@ref ffi_python) — same ABI, ctypes flavour
- [Pure C example](@ref examples_c) — same flow with no FFI shim
- [ABI stability](@ref abi_stability) — what to pin in `Cargo.toml`
