"""Literal sink-v2 contract (sink-v2.md, round 10; wire as in round 5).

No encoding, decoding, checksum or validation code belongs here. The Python
reader and independent test encoder share only these published layout literals.
The report-object inverse is specified in section 21.6.
"""

TAGS = "ETOVKXHI"
MAGIC = b"\x89PFS2\r\n\x1a"
BLOCK_MAGIC = b"BLK2"
SUMMARY_MAGIC = b"SUM2"
PAGE_MAGIC = b"IDX2"
ROOT_MAGIC = b"FTR2"
TAIL_MAGIC = b"PFS2END\n"
VERSION = 2
SAFE = (1 << 53) - 1
DELTA_LIMIT = 1 << 52
DECIMAL_LIMIT = 1 << 50
POW10 = (1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0,
         10000000.0, 100000000.0, 1000000000.0, 10000000000.0, 100000000000.0,
         1000000000000.0)
SCHEMA = "pf-report-sink"
V1_VERSION = 1
INT64_MIN = -(1 << 63)
UINT64_MAX = (1 << 64) - 1
U32_POSITIONS = 1 << 32
MAX_HEAD = 2_097_152
MAX_LINE = 1_048_576
MAX_SUMMARY = 1_048_576  # S_MAX: decoded J and frame window (3.7).
MAX_SUMMARY_STORED = 1_048_609  # Frame allowance: raw fallback at S_MAX (9.1).
MAX_SUMMARY_LENGTH = 1_048_661  # Complete stored SUMMARY: 52+C (3.7).
MAX_DEPTH = 64
MAX_COLUMNS = 699_050  # 32 + 48*ncols <= 33,554,432, before any payload (3.7).
MAX_RECORDS = 65_536
MAX_POSITIONS = 1_048_576
MAX_COLUMN = 16_777_216
MAX_BLOCK = 33_554_432
MAX_STRING = 1_048_575
MAX_BLOCKS = (1 << 32) - 1
HEAD_FIXED = 16
BLOCK_HEADER = 32
DIRECTORY_ENTRY = 16
COLUMN_SHA_SIZE = 32
COLUMN_FRAMING = 48
SUMMARY_HEADER = 52
PAGE_SIZE = 65_536
PAGE_HEADER = 4096
PAGE_PAYLOAD = 61_440
DESCRIPTOR_SIZE = 512
SERIES_SIZE = 192
WITNESS_SIZE = 40
ROOT_SIZE = 4096
COMPACT_ROOT_SIZE = 320
COMPACT_DESCRIPTOR_SIZE = 96
COMPACT_E_LIMIT = 10_980
COMPACT_T_LIMIT = 3314
COMPACT_BLOCK_LIMIT = 128
COMPACT_TABLE_LIMIT = 12_288
COMPACT_FLAG = 1
TAIL_SIZE = 160
TABLES = 19
RANK_TABLE_END = 18
FIGURE_TABLE = 18
FIGURE_BUCKETS = 1200
FIGURE_ENTRY = 256
FIGURE_SELECTORS = 8
FIGURE_WITNESSES = 16
FIGURE_MAX_WITNESSES = 6
ABSENT_SELECTOR = 255
PRESENT = 1
FORCED_CLOSE_EPSILON = 1e-9
LOCATOR_SIZE = 64
DEFAULT_CAPS = dict(zip(TAGS, (4096, 1024, 4096, 4096, 1, 4096, 8192, 65536)))
DEFAULT_BYTES = 1_048_576
MIN_BLOCK_BYTES = 4096
MIN_ZSTD = 64
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
MAX_ZSTD_BLOCK = 131_072
COL_DOMAIN = b"PFS2-COL\0"
BLOCK_DOMAIN = b"PFS2-BLOCK\0"
STREAM_DOMAIN = b"PFS2-STREAM\0"
INDEX_DOMAIN = b"PFS2-INDEX\0"
COMPACT_INDEX_DOMAIN = b"PFS2-CINDEX\0"
NODE_DOMAIN = b"PFS2-NODE\0"
LOGICAL_DOMAIN = b"PFS2-LOGICAL\0"

# Offset, width, and primitive layouts: all fixed integers are little endian.
HEAD_PREFIX = "<8sI"
BLOCK_PREFIX = "<4sBBHIIQI"  # 28 bytes, followed by CRC32
DIRECTORY = "<BBBBIII"
DESCRIPTOR_PREFIX = "<QIBBHIIQqqqq"  # 64 bytes, followed by SHA and summaries
WITNESS = "<QqQQII"
SERIES_PREFIX = "<II"
PAGE_PREFIX = "<4sBBBBIIII"  # 24 bytes, followed by leaf and siblings
LOCATOR = "<QIIQII32s"
TAIL_PREFIX = "<QIIQ"
ROOT_PREFIX = "<4sHHQQII"
ROOT_COMMON_PREFIX = "<4sHH"
COMPACT_ROOT_PREFIX = "<4sHHQII"
SUMMARY_PREFIX = "<4sIIII32s"
COL_PREFIX = "<BIQI"
BLOCK_HASH_PREFIX = "<BQI"
LOGICAL_ITEM = "<BQ"
HEAD_PREFIX_SIZE = 12
BLOCK_CRC = 28
BLOCK_TAG = 4
SUMMARY_JSON_SHA = 20
HASH_SIZE = 32
CRC_SIZE = 4
U64_SIZE = 8
MAX_MERKLE_DEPTH = 32
ENC_PLAIN64 = 0
ENC_DELTA = 1
ENC_XORSHUF = 2
ENC_DECIMAL = 3
ENC_BYTES = 4
ENC_DICT = 5
ENC_PLAINSTR = 6
PAGE_LEAF = 24
PAGE_PROOFS = 56
PAGE_RESERVED = 1080
PAGE_CRC = 4092
ROOT_COUNTS = 32
ROOT_STREAMS = 96
ROOT_SUMMARY_SHA = 352
ROOT_V1_SHA = 384
ROOT_FINITE_E = 416
ROOT_FIGURE_BUCKETS = 424
ROOT_RESERVED = 428
ROOT_TABLES = 512
ROOT_PADDING = 1728
ROOT_CRC = 4092
COMPACT_COUNTS = 24
COMPACT_FINITE_E = 88
COMPACT_SUMMARY_SHA = 96
COMPACT_V1_SHA = 128
COMPACT_TABLE_OFFSET = 160
COMPACT_TABLE_LENGTH = 168
COMPACT_TABLE_SHA = 176
COMPACT_BLOCK_COUNTS = 208
COMPACT_RESERVED = 240
COMPACT_ROOT_CRC = 316
TAIL_ROOT_SHA = 24
TAIL_LOGICAL_SHA = 56
TAIL_STORED_SHA = 88
TAIL_PADDING = 120
TAIL_MAGIC_OFFSET = 152
DESCRIPTOR_SHA = 64
DESCRIPTOR_FLAGS = 13
DESCRIPTOR_MTM = 96
DESCRIPTOR_BOOK = 288
DESCRIPTOR_FINITE_PREFIX = 480
DESCRIPTOR_PADDING = 488
SERIES_WITNESSES = 8
SERIES_PADDING = 168
HEAD_KEYS = ("schema", "version", "run_id", "engine", "records", "format")
FORMAT_KEYS = ("byte_order", "codec", "level", "block_records", "block_bytes")
RANK_KEYS = ("exit", "pnl", "pct", "ru", "dd")
RANK_COLUMNS = (3, 7, 8, 9, 10)
TIME_COLUMNS = {"E": (0, None), "T": (2, 3), "O": (1, 2),
                "V": (3, 4), "X": (0, None), "K": (None, None),
                "H": (None, None), "I": (None, None)}
ENCODINGS = {"i64": (0, 1), "u64": (0, 1), "f64": (2, 3),
             "hex64": (2,), "bool": (4,), "str": (5, 6), "order": (4,)}
SUMMARY_REQUIRED = ("engine", "input", "applied_inputs", "applied_overrides",
                    "applied_runtime", "elapsed_seconds", "summary", "metrics",
                    "diagnostics", "fingerprint", "sink_folds")
SUMMARY_OPTIONAL = ("outputs", "report_window", "report_shape")
SUMMARY_FOLDS = ("closed_count", "open_at_end_count", "open_at_end_pnl")
SUMMARY_FORBIDDEN = ("report_sink", "trades", "equity_curve", "records",
                     "bars", "series", "events", "figure", "figures", "marks",
                     "pdf", "broker_hashes", "hashes", "trace", "traces",
                     "output_rows", "output_bars", "output_series", "output_events")
OUTPUT_METADATA = ("schema_version", "message_format", "manifest_sha256",
                   "manifest", "constants", "hlines")
REPORT_TRADE_FIELDS = ("n", "side", "entry_time", "exit_time", "entry_price",
                       "exit_price", "qty", "pnl", "pnl_pct", "max_runup",
                       "max_drawdown", "commission", "entry_bar_index",
                       "exit_bar_index", "open_at_end", "entry_incarnation")
REPORT_SUMMARY_FIELDS = ("total_trades", "wins", "losses", "win_rate_pct",
                         "net_pnl", "avg_trade", "best_trade", "worst_trade",
                         "max_drawdown", "bars_processed")
REPORT_SUMMARY_FINITE = ("net_pnl", "avg_trade", "best_trade", "worst_trade",
                         "max_drawdown")
REPORT_DIAGNOSTICS = ("input_bars_processed", "script_bars_processed",
                      "magnifier_sub_bars_total", "magnifier_sample_ticks_total",
                      "bar_magnifier_enabled")
REPORT_INPUT_FIELDS = ("ohlcv", "bars", "first_ts", "last_ts", "first_time", "last_time")
OUTPUT_SCHEMA = "pineforge-outputs/v1"
OUTPUT_MESSAGE_FORMAT = "pineforge/v1"
OUTPUT_PHASES = {0: "batch", 1: "warmup", 2: "realtime"}
