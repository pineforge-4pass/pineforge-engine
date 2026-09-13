#!/usr/bin/env python3
"""Narrow offline refusal tests for the settlement ABI tooling; no C++ execution."""
import io
import json
from pathlib import Path
import shutil
import tempfile
import tarfile
from types import SimpleNamespace
import unittest

from check_settlement_cpp_abi import (
    ENGINE, OLD_ENGINE, ROOT, REVERSAL_METHODS, REVERSAL_DOMAIN, archive_engine,
    cross_epoch_rtti_allowed, frozen_shape, load_prior, storage_declarations, validate_rejection,
)
from prepare_settlement_cpp_abi_base import BASE_COMMIT, BASE_TREE, extract_tar, read_cache, PROVIDERS, authenticate_headers


class AbiToolingTests(unittest.TestCase):
    def test_mac_link_diagnostic_requires_only_named_method(self):
        method='project_native_settlement_v1'
        valid=f'Undefined symbols for architecture arm64:\n  "{ENGINE}{method}(int) const", referenced from:\n _main\n'
        self.assertEqual(len(validate_rejection(valid,[method])),1)
        with self.assertRaisesRegex(RuntimeError,'unrelated'):
            validate_rejection(valid+'  "other_dependency()", referenced from:\n _main\n',[method])

    def test_gnu_and_lld_diagnostics_keep_selection_domain(self):
        method='inspect_native_settlement_selected'
        symbol=ENGINE+method+'(pineforge::execution::close_selection_v1::SelectedOpeningSet const&) const'
        for text in ["a.cpp: undefined reference to `"+symbol+"'",'ld.lld: error: undefined symbol: '+symbol]:
            self.assertEqual(len(validate_rejection(text,[method],'close_selection_v1::SelectedOpeningSet')),1)
            with self.assertRaisesRegex(RuntimeError,'namespace'):
                validate_rejection(text,[method],'close_selection_v2::SelectedOpeningSet')

    def test_cross_epoch_rtti_is_exact_and_never_replaces_required_methods(self):
        method = 'project_native_settlement_v1'
        for engine in (ENGINE, OLD_ENGINE):
            owner = engine.removesuffix('::')
            for style in ('mac', 'gnu', 'lld'):
                def diagnostic(symbol):
                    if style == 'mac':
                        return '  "' + symbol + '", referenced from:\n _main\n'
                    if style == 'gnu':
                        return "caller.cpp: undefined reference to `" + symbol + "'\n"
                    return 'ld.lld: error: undefined symbol: ' + symbol + '\n'
                methods = diagnostic(engine + method + '(int) const')
                rtti = diagnostic('typeinfo for ' + owner)
                with self.subTest(engine=engine, style=style):
                    self.assertEqual(len(validate_rejection(methods+rtti, [method],
                        engine=engine, allow_engine_typeinfo=True)), 2)
                    with self.assertRaisesRegex(RuntimeError, 'unrelated'):
                        validate_rejection(methods+rtti, [method], engine=engine)
                    with self.assertRaisesRegex(RuntimeError, 'omits expected'):
                        validate_rejection(rtti, [method], engine=engine,
                                           allow_engine_typeinfo=True)
                    for wrong in ('typeinfo for '+owner+'Other',
                                  'typeinfo for other::BacktestEngine',
                                  'typeinfo for '+(OLD_ENGINE if engine==ENGINE else ENGINE).removesuffix('::'),
                                  'vtable for '+owner, 'other_dependency()'):
                        with self.assertRaisesRegex(RuntimeError, 'unrelated'):
                            validate_rejection(methods+rtti+diagnostic(wrong), [method],
                                engine=engine, allow_engine_typeinfo=True)

    def test_provider_epoch_comes_from_archive_symbols_not_command_line_role(self):
        v13 = '0000000000000100 T ' + OLD_ENGINE + 'inspect_native_settlement(int) const\n'
        v14 = '0000000000000100 T ' + ENGINE + 'inspect_native_settlement_selected(int) const\n'
        other = '0000000000000200 T pineforge::native_order::WorkingRequestCore::reset()\n'
        self.assertEqual(archive_engine(v13 + other), OLD_ENGINE)
        self.assertEqual(archive_engine(other + v14), ENGINE)
        with self.assertRaisesRegex(RuntimeError, 'no BacktestEngine epoch'):
            archive_engine(other)
        with self.assertRaisesRegex(RuntimeError, 'several BacktestEngine epoch'):
            archive_engine(v13 + v14)
        # An authenticated old archive supplied as --library in a partial mode is still v13:
        # the current caller (v14) linking against it is a cross-epoch pair, so sanitized
        # exact-owner RTTI is tolerated exactly as when the same archive arrives by receipt.
        self.assertTrue(cross_epoch_rtti_allowed(ENGINE, archive_engine(v13), True))
        self.assertTrue(cross_epoch_rtti_allowed(OLD_ENGINE, archive_engine(v14), True))
        self.assertFalse(cross_epoch_rtti_allowed(ENGINE, archive_engine(v14), True))
        self.assertFalse(cross_epoch_rtti_allowed(OLD_ENGINE, archive_engine(v13), True))
        for caller, provider in ((ENGINE, v13), (ENGINE, v14), (OLD_ENGINE, v14)):
            self.assertFalse(cross_epoch_rtti_allowed(caller, archive_engine(provider), False))

    def test_generic_failure_or_missing_one_method_cannot_pass(self):
        with self.assertRaisesRegex(RuntimeError,'no recognized'):
            validate_rejection('linker error: file not found',['project_native_settlement_v1'])
        valid=f'undefined reference to `{ENGINE}project_native_settlement_v1(int)\''
        with self.assertRaisesRegex(RuntimeError,'omits expected'):
            validate_rejection(valid,['project_native_settlement_v1','project_native_settlement_scoped_v1'])

    def reversal_diagnostic(self, style, methods=REVERSAL_METHODS, domain=REVERSAL_DOMAIN):
        symbols=[ENGINE+method+'(pineforge::execution::'+domain+' const&, pineforge::execution::Fill const&)'
                 for method in methods]
        formats={'mac': lambda symbol: '  "'+symbol+'", referenced from:\n _main',
                 'gnu': lambda symbol: "caller.cpp: undefined reference to `"+symbol+"'",
                 'lld': lambda symbol: 'ld.lld: error: undefined symbol: '+symbol}
        return '\n'.join(formats[style](symbol) for symbol in symbols)

    def test_reversal_rejection_requires_all_four_names_and_parameter_domain(self):
        for style in ('mac','gnu','lld'):
            with self.subTest(style=style):
                valid=self.reversal_diagnostic(style)
                self.assertEqual(len(validate_rejection(valid,REVERSAL_METHODS,REVERSAL_DOMAIN)),4)
                for omitted in REVERSAL_METHODS:
                    with self.assertRaisesRegex(RuntimeError,'omits expected'):
                        validate_rejection(self.reversal_diagnostic(style,[m for m in REVERSAL_METHODS if m!=omitted]),
                                           REVERSAL_METHODS,REVERSAL_DOMAIN)
                for bad_domain in ('reverse_to_v2::ReverseTo','ReverseTo'):
                    with self.assertRaisesRegex(RuntimeError,'namespace'):
                        validate_rejection(self.reversal_diagnostic(style,domain=bad_domain),
                                           REVERSAL_METHODS,REVERSAL_DOMAIN)
                with self.assertRaisesRegex(RuntimeError,'unrelated'):
                    validate_rejection(valid+"\nundefined reference to `other_dependency()'",
                                       REVERSAL_METHODS,REVERSAL_DOMAIN)

    def test_missing_prior_receipt_fails_with_exact_prepare_remedy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for receipt in (None,root/'missing.json'):
                with self.assertRaisesRegex(RuntimeError,'settlement-abi-prior .*--commit 0e18690'):
                    load_prior(SimpleNamespace(prior_receipt=receipt),root/'headers',{})
            self.assertFalse((root/'headers').exists())

    def test_e60_receipt_cannot_be_used_for_reversal_prior(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);receipt=root/'wrong-provider.json'
            receipt.write_text(json.dumps({'commit':BASE_COMMIT,'tree':BASE_TREE}))
            with self.assertRaisesRegex(RuntimeError,'does not pin 0e18690'):
                load_prior(SimpleNamespace(prior_receipt=receipt),root/'headers',{})
            self.assertFalse((root/'headers').exists())

    def test_cross_epoch_rejection_cannot_match_the_other_epoch(self):
        method='project_native_settlement_v1'
        diagnostic=f"undefined reference to `{OLD_ENGINE}{method}(int)'"
        self.assertEqual(len(validate_rejection(diagnostic,[method],engine=OLD_ENGINE)),1)
        with self.assertRaisesRegex(RuntimeError,'omits expected'):
            validate_rejection(diagnostic,[method])

    def test_real_v13_headers_are_authenticated_and_financial_shape_stays_frozen(self):
        fixture=ROOT/'tests/fixtures/native_cpp_abi/host-c3ed455'
        with tempfile.TemporaryDirectory() as temporary:
            old=Path(temporary)/'v13'
            extract_tar((fixture/'headers.tar').read_bytes(),old)
            provider=PROVIDERS['v13']
            authenticate_headers(old,fixture/'manifest.json',commit=provider['commit'],tree=provider['tree'])
            members,shape=frozen_shape(old/'include',ROOT/'include',selected=True)
            self.assertTrue(shape['epochBreak'])
            self.assertEqual(shape['oldEpoch'],['engine_script_run_v13']*2)
            self.assertEqual(shape['currentEpoch'],['engine_script_run_v14']*2)
            self.assertGreater(len(members),100)

    def test_action_alternative_changes_are_frozen(self):
        with tempfile.TemporaryDirectory() as temporary:
            copied=Path(temporary)/'include'
            shutil.copytree(ROOT/'include',copied)
            header=copied/'pineforge/execution.hpp'
            source=header.read_text()
            original='std::variant<Flatten, order_action::Reduce, order_action::Transact>'
            self.assertIn(original,source)
            header.write_text(source.replace(original,original[:-1]+', int>'))
            with self.assertRaisesRegex(RuntimeError,'Action alternative'):
                frozen_shape(ROOT/'include',copied)

    def test_same_size_member_change_or_added_padding_member_is_visible(self):
        source=(ROOT/'include/pineforge/engine.hpp').read_text()
        original=storage_declarations(source)
        changed=source.replace('double position_entry_price_', 'uint64_t position_entry_price_',1)
        self.assertNotEqual(original,storage_declarations(changed))
        changed=source.replace('double position_entry_price_', 'int abi_padding_member;\n    double position_entry_price_',1)
        self.assertNotEqual(original,storage_declarations(changed))
        self.assertTrue(any('abi_padding_member' in item for item in storage_declarations(changed)))

    def test_tar_links_and_traversal_are_refused_before_extraction(self):
        for name,kind in [('../escape',tarfile.REGTYPE),('link',tarfile.SYMTYPE)]:
            data=io.BytesIO()
            with tarfile.open(fileobj=data,mode='w') as archive:
                info=tarfile.TarInfo(name);info.type=kind;info.linkname='outside'
                archive.addfile(info)
            with tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(RuntimeError,'archive'):
                    extract_tar(data.getvalue(),Path(temporary)/'source')

    def test_cache_preserves_spaces_and_semicolon_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'CMakeCache.txt'
            path.write_text('// comment\nCMAKE_CXX_FLAGS:STRING=-fsanitize=address,undefined -fno-omit-frame-pointer\nCMAKE_PREFIX_PATH:PATH=/one;/two\n')
            found=read_cache(path)
            self.assertEqual(found['CMAKE_CXX_FLAGS'],'-fsanitize=address,undefined -fno-omit-frame-pointer')
            self.assertEqual(found['CMAKE_PREFIX_PATH'],'/one;/two')


if __name__=='__main__':
    unittest.main()
