/* report_sink.h must also parse as C, as existing test targets
 * already compile C callers. This TU is part of test_cr150_contract_headers;
 * it declares no runtime objects/functions and uses no sink implementation.
 * DESIGN revision 6, section 6:1283-1306 and section 3.8:906-907. */
#include <pineforge/report_sink.h>

#include <stddef.h>

_Static_assert(sizeof(pf_report_sink_v1) == 8 + 3 * sizeof(const char*),
               "sink descriptor size changed");
_Static_assert(offsetof(pf_report_sink_v1, struct_size) == 0, "sink size prefix moved");
_Static_assert(offsetof(pf_report_sink_v1, version) == 4, "sink version moved");
_Static_assert(offsetof(pf_report_sink_v1, path) == 8, "sink path moved");
_Static_assert(offsetof(pf_report_sink_v1, run_id) == 8 + sizeof(const char*),
               "sink run_id moved");
_Static_assert(offsetof(pf_report_sink_v1, scratch_dir) == 8 + 2 * sizeof(const char*),
               "sink scratch_dir moved");

_Static_assert(sizeof(int) == 4, "sink fd requires a 32-bit C int");
_Static_assert(offsetof(pf_report_sink_fd_v1, struct_size) == 0, "fd sink size prefix moved");
_Static_assert(offsetof(pf_report_sink_fd_v1, version) == 4, "fd sink version moved");
_Static_assert(offsetof(pf_report_sink_fd_v1, fd) == 8, "sink fd moved");
_Static_assert(offsetof(pf_report_sink_fd_v1, run_id)
               == (12 + _Alignof(const char*) - 1) / _Alignof(const char*) * _Alignof(const char*),
               "fd sink run_id moved");
_Static_assert(sizeof(pf_report_sink_fd_v1)
               == offsetof(pf_report_sink_fd_v1, run_id) + sizeof(const char*),
               "fd sink descriptor size changed");

_Static_assert(sizeof(pf_report_sink_receipt_v1) == 144, "sink receipt size changed");
_Static_assert(offsetof(pf_report_sink_receipt_v1, struct_size) == 0, "receipt size prefix moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, version) == 4, "receipt version moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_e) == 8, "E count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_t) == 16, "T count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_o) == 24, "O count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_v) == 32, "V count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_k) == 40, "K count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_x) == 48, "X count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, count_h) == 56, "H count moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, sha256_hex) == 64, "receipt digest moved");
_Static_assert(sizeof(((pf_report_sink_receipt_v1*)0)->sha256_hex) == 65, "digest size changed");
_Static_assert(offsetof(pf_report_sink_receipt_v1, reserved) == 129, "receipt pad moved");
_Static_assert(sizeof(((pf_report_sink_receipt_v1*)0)->reserved) == 7, "pad size changed");
_Static_assert(offsetof(pf_report_sink_receipt_v1, complete) == 136, "completion flag moved");
_Static_assert(offsetof(pf_report_sink_receipt_v1, trades_retained) == 140, "retention flag moved");
_Static_assert(sizeof(pf_report_sink_summary_v1) == 64, "summary size changed");
_Static_assert(_Alignof(pf_report_sink_summary_v1) == 8, "summary alignment changed");
_Static_assert(offsetof(pf_report_sink_summary_v1, struct_size) == 0, "summary size prefix moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, version) == 4, "summary version moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, count_t) == 8, "summary T count moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, wins) == 16, "summary wins moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, losses) == 24, "summary losses moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, best_trade) == 32, "summary best moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, worst_trade) == 40, "summary worst moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, max_drawdown) == 48, "summary drawdown moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, nonfinite_t_field) == 56, "summary invalid field moved");
_Static_assert(offsetof(pf_report_sink_summary_v1, complete) == 60, "summary completion moved");
_Static_assert(_Generic(&strategy_report_sink_summary_v1,
    int (*)(pf_strategy_t, pf_report_sink_summary_v1*): 1, default: 0),
    "summary query signature changed");
_Static_assert(PF_ABI_VERSION == 6, "strategy module ABI version changed");

_Static_assert(sizeof(pf_equity_point_t) == 24, "scratch point size changed");
_Static_assert(offsetof(pf_equity_point_t, time_ms) == 0, "point timestamp moved");
_Static_assert(offsetof(pf_equity_point_t, equity) == 8, "point equity moved");
_Static_assert(offsetof(pf_equity_point_t, open_profit) == 16, "point open profit moved");
