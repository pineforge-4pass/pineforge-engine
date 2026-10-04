import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PINS = json.loads((ROOT / 'tests/pine_input_domain_gates.json').read_text())


class PineInputDomainGates(unittest.TestCase):
    def test_every_adapter_gate_keeps_its_non_mode_qualifications(self):
        source = (ROOT / 'src/source/pine_adapter.cpp').read_text()
        for pin in PINS:
            with self.subTest(rule=pin['rule'], baseline_line=pin['line']):
                self.assertTrue(pin['window'] in source, pin['rule'])
                self.assertIn(pin['new'], pin['window'])
                old = pin['old']
                if pin['class'] == 'T':
                    expected = old.replace('!stream_mode_', 'modeled_input()').replace(
                        'stream_mode_', '!modeled_input()')
                    expected = expected.replace(
                        'detail::run_state(require_host()).phase == NativeRunPhase::Warmup',
                        '!modeled_input() && detail::run_state(require_host()).phase == NativeRunPhase::Warmup')
                    expected = expected.replace('state.phase == NativeRunPhase::Warmup',
                                                '!modeled_input() && state.phase == NativeRunPhase::Warmup')
                    expected = expected.replace('state.phase != NativeRunPhase::Realtime', 'modeled_input()')
                else:
                    expected = old.replace(' && !stream_mode_', '').replace('!stream_mode_ && ', '')
                    expected = expected.replace('stream_mode_ || ', '').replace(' || stream_mode_', '')
                    expected = expected.replace('state.phase == NativeRunPhase::Batch && ', '')
                    expected = expected.replace(' && state.phase == NativeRunPhase::Batch', '')
                    expected = expected.replace('                && state.phase == NativeRunPhase::Batch', '')
                self.assertEqual(pin['new'], expected)
                self.assertTrue(pin['evidence'])

    def test_wave_gates_are_measured_pins(self):
        pinned = {pin['line'] for pin in PINS}
        self.assertTrue({14178, 15016, 18762, 21206}.issubset(pinned))

    def test_sibling_touch_uses_the_modeled_input_domain(self):
        source = (ROOT / 'src/source/pine_adapter.cpp').read_text()
        self.assertTrue('            && leftover_flat_stop\n'
                      '            && policy_script_bar_valid_\n'
                      '            && !config_.calc_on_order_fills\n'
                      '            && modeled_input()) {' in source)

    def test_pairless_callback_keys_on_the_open_hook_epoch(self):
        source = (ROOT / 'src/source/pine_adapter.cpp').read_text()
        self.assertTrue('    if (last_bar_dual_entry_script_open_ms_ != context.script_bar_open_ms) {\n'
                        '        last_bar_dual_entry_path_ = 0;' in source)

    def test_input_domain_depends_only_on_observed_provenance(self):
        header = (ROOT / 'include/pineforge/source/pine_input_domain.hpp').read_text()
        self.assertIn('return provenance != NativePriceProvenance::ObservedPrint;', header)
        self.assertNotIn('is_stream', header)

    def test_tick_derived_close_calculations_keep_the_observed_input_domain(self):
        source = (ROOT / 'src/source/pine_adapter.cpp').read_text()
        self.assertIn('if (input_scheduler_ && input_scheduler_->input_is_observed_ticks())', source)
        self.assertIn('input_scheduler_ = pine_host ? &pine_host->scheduler_ : nullptr;', source)
        predicate = source.split('bool PineExecutionAdapter::modeled_input() const {', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('dynamic_cast', predicate)
        self.assertNotIn('stream_mode_', predicate)
        scheduler = (ROOT / 'src/source/pine_scheduler_native.cpp').read_text()
        self.assertIn('input_is_observed_ticks_ = true;', scheduler)
        self.assertIn('input_is_observed_ticks_ = false;', scheduler)
        hashing = (ROOT / 'src/source/pine_state_hash.cpp').read_text()
        self.assertIn('if (retained_.is_stream) f.b(input_is_observed_ticks_);', hashing)

    def test_known_confirmed_script_label_is_not_rewritten_at_handoff(self):
        source = (ROOT / 'src/source/pine_scheduler_native.cpp').read_text()
        self.assertIn('script_bar.timestamp = context.script_bar_open_ms;', source)
        self.assertNotIn('script_bar.timestamp = expected_open;', source)

    def test_session_setter_does_not_discard_warmup_settings(self):
        source = (ROOT / 'src/source/pine_strategy_host.cpp').read_text()
        self.assertIn('void source::PineStrategyHost::set_syminfo_session(const std::string& session) {\n'
                      '    BacktestEngine::set_syminfo_session(session);\n}', source)


if __name__ == '__main__':
    unittest.main()
