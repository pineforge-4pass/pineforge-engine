# Caps, refusals and defaults {#caps_and_defaults}

This table is generated from the engine source and the small manifest beside
the generator. It describes the limits and sizing defaults a host needs when
admitting a run. `0` means unlimited only where the row says so. `RLIMIT`
settings and `memory_limit` are host controls; the engine does not enforce
them.

<!-- BEGIN generated cap table: python3 scripts/gen_cap_table.py --write -->

| Row | Source value | Unit | Enforced where | Refusal | Unlimited | Who may change it |
| --- | --- | --- | --- | --- | --- | --- |
| `scratch-layout-page-size` | `4096` (include/pineforge/detail/scratch_slab.hpp:19) | bytes per scratch page | compute_layout page rounding and ScratchSlab alignment | `scratch_capacity_exceeded` / `layout` | no | compile-time |
| `scratch-region-count` | `9` (include/pineforge/detail/scratch_slab.hpp:18) | regions | ScratchSlab Layout region array | `scratch_capacity_exceeded` / `layout` | no | compile-time |
| `reserved-sequence-key-count` | `0` (include/pineforge/detail/scratch_defaults.hpp:28) | keys | compute_layout requires the removed q region to remain zero | `scratch_capacity_exceeded` / `layout` | no; reserved zero is invalid when nonzero | compile-time default |
| `reserved-sequence-byte-count` | `0` (include/pineforge/detail/scratch_defaults.hpp:28) | bytes | compute_layout requires the removed q region to remain zero | `scratch_capacity_exceeded` / `layout` | no; reserved zero is invalid when nonzero | compile-time default |
| `erased-key-count-default` | `262144` (include/pineforge/detail/scratch_defaults.hpp:29) | keys | DiskIdTable admission before erased-key growth | `scratch_capacity_exceeded` / `erased_data` | no | compile-time default |
| `erased-key-bytes-default` | `25165824` (include/pineforge/detail/scratch_defaults.hpp:29) | bytes | IdPairStore erased-data admission | `scratch_capacity_exceeded` / `erased_index` | no | compile-time default |
| `consumed-key-count-default` | `32768` (include/pineforge/detail/scratch_defaults.hpp:30) | keys | IdPairStore consumed-key admission | `scratch_capacity_exceeded` / `consumed_data` | no | compile-time default |
| `consumed-key-bytes-default` | `4227072` (include/pineforge/detail/scratch_defaults.hpp:30) | bytes | IdPairStore consumed-key admission | `scratch_capacity_exceeded` / `consumed_index` | no | compile-time default |
| `cohort-count-default` | `65536` (include/pineforge/detail/scratch_defaults.hpp:31) | keys | DiskIdTable and resident cohort admission | `scratch_capacity_exceeded` / `cohort` | no | compile-time default |
| `cohort-byte-default` | `4194304` (include/pineforge/detail/scratch_defaults.hpp:31) | bytes | DiskIdTable cohort byte admission | `scratch_capacity_exceeded` / `cohort` | no | compile-time default |
| `exit-rank-count-default` | `27` (include/pineforge/detail/scratch_defaults.hpp:32) | keys | compute_layout and exit-rank admission | `scratch_capacity_exceeded` / `exit_rank` | no | compile-time default |
| `exit-rank-byte-default` | `6912` (include/pineforge/detail/scratch_defaults.hpp:32) | bytes | exit-rank admission | `scratch_capacity_exceeded` / `exit_rank` | no | compile-time default |
| `successor-root-reserve-default` | `2 * bars` (include/pineforge/detail/scratch_defaults.hpp:33) | rows | successor-root slab region before row writes | `scratch_capacity_exceeded` / `successor_roots` | no | compile-time default |
| `successor-root-stream-default` | `65536` (include/pineforge/detail/scratch_defaults.hpp:41) | rows | default stream scratch manifest | `scratch_capacity_exceeded` / `successor_roots` | no | compile-time default |
| `layout-manifest-v-ceiling` | `manifest.V > 27` (include/pineforge/detail/scratch_slab.hpp:77) | manifest exit-rank keys | compute_layout manifest validation | `scratch_capacity_exceeded` / `layout` | no | compile-time |
| `identifier-component-max-cohort` | `key_size > 64` (include/pineforge/detail/disk_id_table.hpp:208) | bytes per identifier component | DiskIdTable::require_id_size for cohort tables | `scratch_capacity_exceeded` / `cohort` | no | compile-time policy literal |
| `identifier-component-max-pair` | `first > 64 \|\| second > 64` (include/pineforge/detail/id_pair_store.hpp:428) | bytes per identifier component | IdPairStore::require_id_components | `scratch_capacity_exceeded` / `erased_data` | no | compile-time policy literal |
| `resident-cohort-cap` | `4096` (include/pineforge/detail/scratch_defaults.hpp:16) | non-spillable live order ids | DiskResidentIdTable::require_live_growth | `scratch_capacity_exceeded` / `cohort` | no | compile-time |
| `o1-min-page-count` | `2` (include/pineforge/detail/scratch_store.hpp:15) | resident pages | StoreImpl geometry validation | none: engine exception is outside the run-failure catalog | no | compile-time |
| `o1-min-page-size` | `512` (include/pineforge/detail/scratch_store.hpp:16) | bytes per page | StoreImpl geometry validation | none: engine exception is outside the run-failure catalog | no | compile-time |
| `o1-max-chunk-bytes` | `64u * 1024u` (include/pineforge/detail/o1_store.hpp:56) | bytes per object chunk | StoreImpl geometry validation | none: engine exception is outside the run-failure catalog | no | compile-time |
| `o1-max-resident-pages` | `1024u` (include/pineforge/detail/o1_store.hpp:57) | resident pages | StoreImpl geometry validation | none: engine exception is outside the run-failure catalog | no | compile-time |
| `o1-transaction-handle-pool` | `4` (include/pineforge/detail/o1_store.hpp:64) | caller transaction handles | TxPool allocation | none: internal exception is outside the run-failure catalog | no | compile-time |
| `shape-page-bytes-default` | `4096` (include/pineforge/detail/o1_store.hpp:84) | bytes per page | StoreImpl open and backing page geometry | none: engine exception is outside the run-failure catalog | no | Shape caller |
| `shape-resident-pages-default` | `64` (include/pineforge/detail/o1_store.hpp:85) | resident pages | StoreImpl ScratchStoreConfig | none: engine exception is outside the run-failure catalog | no | Shape caller |
| `shape-chunk-bytes-default` | `1024` (include/pineforge/detail/o1_store.hpp:89) | bytes per object chunk | StoreImpl object chunking | none: engine exception is outside the run-failure catalog | no | Shape caller |
| `shape-journal-slots-default` | `8` (include/pineforge/detail/o1_store.hpp:92) | resident journal slots | StoreImpl normalizes zero to one | none: zero is normalized by the engine | no | Shape caller |
| `shape-index-slots-default` | `8` (include/pineforge/detail/o1_store.hpp:93) | resident index slots | StoreImpl normalizes zero to one | none: zero is normalized by the engine | no | Shape caller |
| `shape-live-object-limit-default` | `0` (include/pineforge/detail/o1_store.hpp:97) | live objects per store | StoreImpl::create | `scratch_capacity_exceeded` / `store_objects` | 0 means no count limit | Shape caller |
| `scratch-store-default-page-count` | `0` (include/pineforge/detail/scratch_store.hpp:35) | resident cache pages | CommandScratchStore geometry validation | none: engine status is outside the run-failure catalog | no | host configuration |
| `scratch-store-default-page-size` | `0` (include/pineforge/detail/scratch_store.hpp:36) | bytes per page | CommandScratchStore geometry validation | none: engine status is outside the run-failure catalog | no | host configuration |
| `scratch-store-default-journal-slots` | `0` (include/pineforge/detail/scratch_store.hpp:37) | resident staging slots | CommandScratchStore geometry validation | none: engine status is outside the run-failure catalog | no | host configuration |
| `scratch-store-default-index-slots` | `0` (include/pineforge/detail/scratch_store.hpp:38) | resident index staging slots | CommandScratchStore geometry validation | none: engine status is outside the run-failure catalog | no | host configuration |
| `scratch-store-default-mode` | `ScratchOpenMode::reopen` (include/pineforge/detail/scratch_store.hpp:32) | open mode | CommandScratchStore open contract | none: engine status is outside the run-failure catalog | no | host configuration |
| `scratch-store-max-page-count` | `std::size_t{1} << 24` (include/pineforge/detail/scratch_store_impl.hpp:152) | backing pages | scratch_store_impl geometry validation | none: engine status is outside the run-failure catalog | no | compile-time |
| `scratch-store-max-page-size` | `std::size_t{1} << 20` (include/pineforge/detail/scratch_store_impl.hpp:153) | bytes per page | scratch_store_impl geometry validation | none: engine status is outside the run-failure catalog | no | compile-time |
| `o1-object-header-bytes` | `16` (src/detail/o1_store.cpp:50) | bytes per object header | StoreImpl object chunk records | none: not enforced by the engine | no | compile-time |
| `script-string-chunk-bytes` | `4096` (include/pineforge/script_string.hpp:44) | bytes per string chunk | ScriptString storage | none: not enforced by the engine | no | compile-time |
| `arena-chunk-bytes` | `1024ULL * 1024ULL` (include/pineforge/detail/scratch_arena.hpp:112) | bytes per arena chunk | ScratchArena allocation and mapping geometry | `report_sink_failed` | no | compile-time |
| `arena-default-window-bytes` | `32ULL * kChunkBytes` (include/pineforge/detail/scratch_arena.hpp:113) | bytes mapped at once | ScratchArena attach environment default | `report_sink_failed` | no | compile-time default; PF_SCRATCH_WINDOW_BYTES may override |
| `arena-inherited-fd` | `5` (include/pineforge/detail/scratch_arena.hpp:114) | file descriptor number | ScratchArena::attach_from_environment | `scratch_configuration_invalid` | no | compile-time ABI; host supplies descriptor |
| `scratch-file-cache-bytes` | `64 * 1024` (include/pineforge/detail/scratch_file.hpp:40) | bytes per cache window | ScratchFile bounded-cache read/write windows | none: no capacity refusal; I/O uses the typed backend error | no | compile-time |
| `cached-backing-cache-bytes` | `64 * 1024` (src/command_scratch_cache.hpp:24) | bytes of cache storage | CachedScratchBacking fixed page cache | `scratch_io_failed` | no | compile-time |
| `cached-backing-min-page-bytes` | `512` (src/command_scratch_cache.hpp:51) | bytes per minimum page | CachedScratchBacking cache geometry | none: pages below the floor bypass the cache | no | compile-time |
| `cached-backing-max-slots` | `kCacheBytes / kMinPageBytes` (src/command_scratch_cache.hpp:52) | cache slots | CachedScratchBacking page-id array | `scratch_io_failed` | no | compile-time |
| `ledger-record-bytes` | `168` (include/pineforge/trade_ledger.hpp:37) | bytes per numeric ledger record | TradeLedger cache accounting | none: not enforced by the engine | no | compile-time |
| `ledger-resident-rows` | `64` (include/pineforge/trade_ledger.hpp:38) | rows | TradeLedger resident descriptor window | none: not enforced by the engine | no | compile-time |
| `ledger-cache-string-bytes` | `128` (include/pineforge/trade_ledger.hpp:45) | inline string bytes per row | TradeLedger cache accounting | none: not enforced by the engine | no | compile-time |
| `ledger-entry-bytes` | `kRecordBytes + 2 * sizeof(size_type) + kCacheStringBytes` (include/pineforge/trade_ledger.hpp:46) | bytes per cached entry | TradeLedger cache accounting | none: not enforced by the engine | no | compile-time |
| `ledger-default-cache-rows` | `65536` (include/pineforge/trade_ledger.hpp:48) | rows | TradeLedger cache setter default | none: engine exception is outside the run-failure catalog | no | compile-time default; host setter may change |
| `ledger-max-cache-rows` | `1048576` (include/pineforge/trade_ledger.hpp:49) | rows | TradeLedger cache setter and engine_orders row-window validation | none: engine exception is outside the run-failure catalog | no | compile-time |
| `ledger-sink-cache-rows` | `128` (include/pineforge/trade_ledger.hpp:52) | rows | TradeLedger report-sink cache policy | none: not enforced by the engine | no | compile-time |
| `report-sink-safe-integer` | `INT64_C(9007199254740991)` (src/report_sink_writer.cpp:31) | exact JSON integer magnitude | report sink integer serialization | none: writer exception is outside the run-failure catalog | no | compile-time wire contract |
| `report-sink-max-line` | `1024 * 1024` (src/report_sink_writer.cpp:32) | bytes per line including LF | report sink Line buffer and field checks | none: writer exception is outside the run-failure catalog | no | compile-time wire contract |
| `report-sink-io-window` | `64 * 1024` (src/report_sink_writer.cpp:33) | bytes per flush window | report sink stream writer | none: not enforced by the engine | no | compile-time |
| `report-sink-io-alignment` | `4096` (src/report_sink_writer.cpp:34) | bytes of buffer alignment allowance | report sink aligned buffer storage | none: not enforced by the engine | no | compile-time |
| `curve-window-bytes` | `64 * 1024` (src/metrics_spill_curve.cpp:26) | bytes per curve read window | read_scratch_curve_region | none: not enforced by the engine | no | compile-time |
| `curve-window-points` | `static_cast<std::int64_t>(window_bytes / sizeof(pf_equity_point_t))` (src/metrics_spill_curve.cpp:27) | points per curve read window | read_scratch_curve_region | none: not enforced by the engine | no | compile-time derived |
| `curve-point-bytes` | `sizeof(pf_equity_point_t)` (src/metrics_spill_curve.cpp:29) | bytes per equity point | read_scratch_curve_region offset arithmetic | none: not enforced by the engine | no | compile-time derived |
| `host-rlimit-as` | not a source cap | host address-space limit | host process policy; mapped windows are sized against it | none: not enforced by the engine | host-defined | host ABI / operating system |
| `host-rlimit-fsize` | not a source cap | host file-size limit | host process policy; engine converts resulting I/O to typed failure where applicable | none: not enforced by the engine | host-defined | host ABI / operating system |
| `host-memory-limit` | not a source cap | host memory limit | host policy only | none: not enforced by the engine | host-defined | host ABI |

<!-- END generated cap table -->
