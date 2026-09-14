#!/usr/bin/env python3
"""Narrow offline refusal tests for the settlement ABI tooling; no C++ execution."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock

import check_settlement_cpp_abi as checker
from check_settlement_cpp_abi import (
    ENGINE, CURRENT_EPOCH, OLD_EPOCHS, PROVIDER_ORDER_SHAPES,
    EPOCH_TRANSITION_HEADER_EXEMPTIONS, FROZEN_NATIVE_HEADERS, OLD_ENGINE, ROOT,
    REVERSAL_METHODS, REVERSAL_DOMAIN, archive_engine, compare_layout_words,
    cross_epoch_rtti_allowed, frozen_native_header_exemptions, frozen_shape, link_outcome,
    load_prior, load_provider, provider_engine_for, storage_declarations, validate_rejection,
    EXEMPTED_HEADER_SHA256, verify_exempted_header_pins,
    COMMON, provider_order_shape, render_provider_caller, native_domain_callers,
    pending_surface_rows, normalized, relocation_manifest, layout_source,
)
from prepare_settlement_cpp_abi_base import BASE_COMMIT, BASE_TREE, extract_tar, read_cache, PROVIDERS, authenticate_headers


class AbiToolingTests(unittest.TestCase):
    def test_current_epoch_and_provider_relative_variant_pins(self):
        self.assertEqual(CURRENT_EPOCH, 'engine_script_run_v16')
        self.assertEqual(OLD_EPOCHS, ('engine_script_run_v13','engine_script_run_v14'))
        self.assertEqual(PROVIDER_ORDER_SHAPES, {
            'engine_script_run_v13': (16,3), 'engine_script_run_v14': (16,3),
            'engine_script_run_v15': (17,5),
            CURRENT_EPOCH: (checker.CURRENT_ORDER_VARIANT,checker.CURRENT_ORDER_INTENT_VARIANT)})
        self.assertEqual(provider_order_shape(ROOT/'include'), (17,5))
        rendered = render_provider_caller(COMMON, ROOT/'include')
        self.assertIn('CommandEvent> == 17',rendered)
        self.assertIn('OrderIntent> == 5',rendered)
        self.assertNotIn('COMMAND_EVENT_ALTERNATIVES',rendered)
        self.assertNotIn('ORDER_INTENT_ALTERNATIVES',rendered)

    def test_v15_v16_manifest_is_exact_and_uses_the_source_pending_row(self):
        manifest = relocation_manifest()
        self.assertEqual(manifest['rejectionPairs'], [
            ['v15-frozen', 'v16-current'], ['v16-current', 'v15-frozen']])
        fixture = PROVIDERS['v15-frozen']
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = root/'old'
            extract_tar((fixture['manifest'].parent/'headers.tar').read_bytes(), old)
            current = root/'current'
            shutil.copytree(ROOT/'include', current)
            members, shape = frozen_shape(old/'include', current, selected=True)
            self.assertTrue(shape['relocationLayout'])
            self.assertEqual(set(shape['removedStorage']), set(manifest['removedStorage']))
            self.assertEqual(set(shape['addedVirtuals']), set(manifest['addedVirtuals']))
            self.assertEqual(shape['removedVirtuals'], [])
            source, width = layout_source(
                members, source_pending=True, relocation_layout=True)
            self.assertIn('#include <pineforge/source/pine_pending_intent.hpp>', source)
            self.assertIn('sizeof(pineforge::source::PendingOrder)', source)
            self.assertNotIn('sizeof(E)', source)
            self.assertGreater(width, 0)

            header = current/'pineforge/engine.hpp'
            original = header.read_text()
            self.assertIn('double initial_capital_', original)
            header.write_text(original.replace(
                'double initial_capital_', 'int unlisted_storage_;\n    double initial_capital_', 1))
            with self.assertRaisesRegex(RuntimeError, 'relocation manifest does not exactly describe removed storage'):
                frozen_shape(old/'include', current, selected=True)

            header.write_text(original.replace(
                'virtual ~BacktestEngine();',
                'virtual void unlisted_virtual_seam();\n    virtual ~BacktestEngine();', 1))
            with self.assertRaisesRegex(RuntimeError, 'relocation manifest does not exactly describe vtable deltas'):
                frozen_shape(old/'include', current, selected=True)

            header.write_text(original.replace(
                'virtual void reset_source_pending_book();',
                'void reset_source_pending_book();', 1))
            with self.assertRaisesRegex(RuntimeError, 'relocation manifest does not exactly describe vtable deltas'):
                frozen_shape(old/'include', current, selected=True)

    def test_all_frozen_host_epochs_authenticate_and_keep_their_own_shapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for role, expected_order_shape in (('v13',(16,3)), ('v14',(16,3)), ('v15-frozen',(17,5))):
                provider = PROVIDERS[role]
                fixture = provider['manifest'].parent
                old = root/role
                extract_tar((fixture/provider['headers_name']).read_bytes(),old)
                authenticate_headers(old,provider['manifest'],commit=provider['commit'],tree=provider['tree'])
                self.assertEqual(provider_order_shape(old/'include'),expected_order_shape)
                rendered = render_provider_caller(COMMON,old/'include')
                self.assertIn('CommandEvent> == '+str(expected_order_shape[0]),rendered)
                self.assertIn('OrderIntent> == '+str(expected_order_shape[1]),rendered)
                _,shape = frozen_shape(old/'include',ROOT/'include',selected=True)
                self.assertEqual(shape['oldEpoch'],[provider['engine_epoch']]*2)
                self.assertEqual(shape['currentEpoch'],[CURRENT_EPOCH]*2)
                if role == 'v14':
                    before = (old/'include/pineforge/native_order_identity.hpp').read_text()
                    after = (ROOT/'include/pineforge/native_order_identity.hpp').read_text()
                    self.assertNotEqual(before,after)
                    self.assertEqual(normalized(before),normalized(after))
                # Counts come from the provider's header and cannot self-authorize
                # a changed layout merely because the epoch token remains intact.
                header = old/'include/pineforge/native_order.hpp'
                original_variant = ('std::variant<Flatten, Reduce, Transact, ReverseTo, HostSized>'
                                    if role == 'v15-frozen'
                                    else 'std::variant<Flatten, Reduce, Transact>')
                header.write_text(header.read_text().replace(original_variant,
                                                              'std::variant<Flatten, Reduce>'))
                with self.assertRaisesRegex(RuntimeError,'unreviewed provider order shape'):
                    provider_order_shape(old/'include')

    def test_domain_pairs_preserve_both_unchanged_driver_cross_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            domains = {'v16':native_domain_callers(ROOT/'include')}
            for role in ('v13','v14','v15-frozen'):
                provider = PROVIDERS[role]
                extract_tar((provider['manifest'].parent/'headers.tar').read_bytes(),root/role)
                domains[role] = native_domain_callers(root/role/'include')
            self.assertIn('native_order_v3',domains['v14']['order'][1])
            self.assertIn('native_order_v4',domains['v16']['order'][1])
            for caller in domains:
                for provider in domains:
                    for domain in domains[caller]:
                        actual = domains[caller][domain][2] == domains[provider][domain][2]
                        expected = (caller == provider
                                    or (domain == 'order'
                                        and {caller,provider} <= {'v16','v15-frozen'})
                                    or (domain == 'driver'
                                        and {caller,provider} <= {'v14','v16','v15-frozen'}))
                        self.assertEqual(actual,expected,(caller,provider,domain))

    def test_pending_surface_rows_are_complete_and_current_only(self):
        self.assertTrue(checker.CURRENT_TERMS_SURFACE_READY)
        rows = pending_surface_rows('v16',('v13','v14','v15-frozen','v16'),False)
        self.assertEqual({row['name'] for row in rows}, {
            'v16-'+caller+'-'+provider for caller in ('current-execution-terms','native-fx-curve')
            for provider in ('v13','v14','v15-frozen','v16')})
        self.assertTrue(all(row['status']=='pending-surface' and row['caller']=='v16' for row in rows))
        self.assertTrue(all(len(row['sourceSha256'])==64 for row in rows))
        self.assertEqual(pending_surface_rows('v16',('v13','v14','v15-frozen','v16'),True),[])
        from check_native_cpp_abi import render_current_execution_caller, control_applicability
        for epoch in ('engine_script_run_v14',CURRENT_EPOCH):
            self.assertIn(epoch+'::NativeStrategyHost',render_current_execution_caller(epoch))
        with self.assertRaises((RuntimeError,ValueError)):
            render_current_execution_caller('engine_script_run_v13')
        controls = {row['name']:row for row in control_applicability(False)}
        self.assertEqual(controls['v14_current_execution_shape_agnostic_compile']['status'],'required')
        for name in ('v16_current_execution_surface_compile','v16_current_result_missing_cancelled_compile_reject',
                     'v16_native_fx_curve_surface_compile',
                     'v16_to_v15_frozen_current_execution_compile_reject',
                     'v16_to_v15_frozen_native_fx_curve_compile_reject'):
            self.assertEqual(controls[name]['status'],'pending_surface')
        self.assertTrue(all(row['status']=='required' for row in control_applicability(True)))

    def test_loader_authenticates_then_checks_actual_owner_present_and_absent_symbols(self):
        # These deliberately fake archive bytes exercise loader refusals only;
        # the registered C++ matrix still requires four full historical builds.
        for role in ('v13','v14'):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                provider = PROVIDERS[role]
                library = root/'lib.a'
                library.write_bytes(b'!<arch>\nunit-test-only')
                headers = root/'headers.tar'
                shutil.copyfile(provider['manifest'].parent/'headers.tar',headers)
                generated = root/'generated/pineforge'
                generated.mkdir(parents=True)
                (generated/'version.h').write_text('// unit-test generated version\n')
                identity = checker.identity
                compiler = {'target':'test','sha256':'compiler-test','version':'test'}
                receipt = root/'receipt.json'
                receipt_data = {
                    'schemaVersion':'pineforge-settlement-abi-base/v1',
                    'commit':provider['commit'],'tree':provider['tree'],
                    'archive':library.name,'archiveSha256':identity(library)['sha256'],
                    'headers':headers.name,'headersSha256':identity(headers)['sha256'],
                    'compiler':compiler,'copiedCurrentCache':{},'generatedInclude':'generated',
                    'generatedHeaderSha256':identity(generated/'version.h')['sha256']}
                receipt.write_text(json.dumps(receipt_data))
                owner = 'pineforge::'+provider['engine_epoch']+'::BacktestEngine::'
                other = ENGINE
                symbols = '0000 T '+owner+'present()\n'
                cases = [
                    ('present',symbols,None,provider),
                    ('absent',symbols+'0001 T '+owner+'forbidden()\n','already exports new method',provider),
                    ('missing-method','0001 T '+owner+'other()\n','omits original symbol',provider),
                    ('mixed',symbols+'0002 T '+other+'other()\n','several BacktestEngine epoch',provider),
                    ('missing-epoch','0002 T other_dependency()\n','no BacktestEngine epoch',provider),
                    ('wrong-archive','0002 T '+other+'present()\n','archive owner differs',provider),
                    ('mislabeled',symbols,'header epoch differs',{**provider,'engine_epoch':CURRENT_EPOCH}),
                ]
                for name,defined,error,role_pin in cases:
                    with self.subTest(case=name), mock.patch.object(checker,'defined_symbols',return_value=defined), \
                            mock.patch.object(checker,'compiler_identity',return_value=compiler), \
                            mock.patch.object(checker,'run',return_value=b'product.o\n'*20):
                        arguments = (SimpleNamespace(compiler='test'),root/name,{},receipt,role_pin)
                        if error:
                            with self.assertRaisesRegex(RuntimeError,error):
                                load_provider(*arguments,expect_present=('present',),expect_absent=('forbidden',))
                        else:
                            result = load_provider(*arguments,expect_present=('present',),expect_absent=('forbidden',))
                            self.assertEqual(result[0],library.resolve())
                # Receipt and header authentication must fail before symbol inspection.
                receipt.write_text(json.dumps({**receipt_data,'headersSha256':'0'*64}))
                with mock.patch.object(checker,'defined_symbols') as reader:
                    with self.assertRaisesRegex(RuntimeError,'bytes do not match receipt'):
                        load_provider(SimpleNamespace(),root/'corrupt',{},receipt,provider,
                                      expect_present=(),expect_absent=())
                    reader.assert_not_called()

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
        v15 = '0000000000000100 T ' + ENGINE + 'inspect_native_settlement_selected(int) const\n'
        other = '0000000000000200 T pineforge::native_order::WorkingRequestCore::reset()\n'
        self.assertEqual(archive_engine(v13 + other), OLD_ENGINE)
        self.assertEqual(archive_engine(other + v15), ENGINE)
        with self.assertRaisesRegex(RuntimeError, 'no BacktestEngine epoch'):
            archive_engine(other)
        with self.assertRaisesRegex(RuntimeError, 'several BacktestEngine epoch'):
            archive_engine(v13 + v15)
        # An authenticated old archive supplied as --library in a partial mode is still v13:
        # the current caller (v15) linking against it is a cross-epoch pair, so sanitized
        # exact-owner RTTI is tolerated exactly as when the same archive arrives by receipt.
        self.assertTrue(cross_epoch_rtti_allowed(ENGINE, archive_engine(v13), True))
        self.assertTrue(cross_epoch_rtti_allowed(OLD_ENGINE, archive_engine(v15), True))
        self.assertFalse(cross_epoch_rtti_allowed(ENGINE, archive_engine(v15), True))
        self.assertFalse(cross_epoch_rtti_allowed(OLD_ENGINE, archive_engine(v13), True))
        for caller, provider in ((ENGINE, v13), (ENGINE, v15), (OLD_ENGINE, v15)):
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
            self.assertEqual(shape['currentEpoch'],['engine_script_run_v16']*2)
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

    def mac_undefined(self, *symbols):
        return 'Undefined symbols for architecture arm64:\n' + ''.join(
            '  "'+symbol+'", referenced from:\n _main\n' for symbol in symbols)

    def test_old_rejections_only_library_alias_decides_rtti_through_link_outcome(self):
        # review-2's exact configuration: a sanitized build running
        # --old-rejections-only, where --library is an alias for the real base
        # (epoch 13) archive. Nothing but the archive's own defined symbols may
        # decide that the v15 caller is a cross-epoch pair.
        method = 'project_native_settlement_v1'
        v13_symbols = '0000000000000100 T ' + OLD_ENGINE + 'inspect_native_settlement(int) const\n'
        v15_symbols = '0000000000000100 T ' + ENGINE + method + '(int) const\n'
        reads = []

        def reader(symbols):
            def read(path):
                reads.append(path)
                return symbols
            return read

        cache = {}
        aliased = Path('/nonexistent/settlement-abi-base/build/lib/libpineforge.a')
        current = Path('/nonexistent/build/lib/libpineforge.a')
        v13_reader = reader(v13_symbols)
        self.assertEqual(provider_engine_for(aliased, cache, v13_reader), OLD_ENGINE)
        self.assertEqual(provider_engine_for(aliased, cache, v13_reader), OLD_ENGINE)
        self.assertEqual(reads, [aliased])  # memoized: one archive read per runtime path
        self.assertEqual(provider_engine_for(current, cache, reader(v15_symbols)), ENGINE)
        self.assertEqual(provider_engine_for(current, cache, reader(v15_symbols)), ENGINE)
        self.assertEqual(reads, [aliased, current])

        diagnostic = self.mac_undefined(ENGINE + method + '(int) const',
                                        'typeinfo for ' + ENGINE.removesuffix('::'))
        entry = link_outcome('new-api-old-real-rejected', 1, diagnostic, [method], None, ENGINE,
                             None, provider_engine_for(aliased, cache, v13_reader), True)
        self.assertEqual(entry['providerEngine'], OLD_ENGINE)
        self.assertEqual(entry['outcome'], 'expected-rejection')
        self.assertEqual(entry['requiredMissing'], [method])
        self.assertFalse(entry['executed'])
        for provider_engine, sanitizers_on in ((ENGINE, True), (OLD_ENGINE, False), (ENGINE, False)):
            with self.subTest(provider=provider_engine, sanitizers=sanitizers_on):
                with self.assertRaisesRegex(RuntimeError, 'unrelated'):
                    link_outcome('new-api-old-real-rejected', 1, diagnostic, [method], None, ENGINE,
                                 None, provider_engine, sanitizers_on)
        with self.assertRaisesRegex(RuntimeError, 'unexpectedly linked'):
            link_outcome('new-api-old-real-rejected', 0, '', [method], None, ENGINE, None,
                         OLD_ENGINE, True)
        with self.assertRaisesRegex(RuntimeError, 'positive pair failed'):
            link_outcome('new-api-new-real', 1, 'ld: symbol not found', (), None, ENGINE, None,
                         ENGINE, True)
        linked = link_outcome('new-api-new-real', 0, '', (), None, ENGINE, None, ENGINE, True)
        self.assertEqual((linked['outcome'], linked['providerEngine']), ('linked', ENGINE))
        epoch_symbol = 'pineforge::engine_script_run_v13::NativeStrategyHost::native_events('
        rejected = link_outcome('old-events-new-real-epoch-rejected', 1,
                                self.mac_undefined(epoch_symbol + ')'), (), None, ENGINE,
                                epoch_symbol, ENGINE, True)
        self.assertEqual(rejected['requiredEpochSymbol'], epoch_symbol)
        self.assertEqual(rejected['outcome'], 'expected-rejection')
        with self.assertRaisesRegex(RuntimeError, 'lacks expected epoch symbol'):
            link_outcome('old-events-new-real-epoch-rejected', 1,
                         self.mac_undefined('other_dependency()'), (), None, ENGINE,
                         epoch_symbol, ENGINE, True)

    def native_headers(self, root, **changed):
        directory = root/'pineforge'
        directory.mkdir(parents=True)
        for name in FROZEN_NATIVE_HEADERS:
            body = changed.get(name, 'struct Frozen { int one; };')
            (directory/name).write_text('// '+name+'\n'+body+'\n')
        return root

    def test_exempted_headers_are_pinned_to_their_reviewed_bytes(self):
        transition = ('engine_script_run_v13', 'engine_script_run_v15')
        self.assertEqual(set(EXEMPTED_HEADER_SHA256), set(EPOCH_TRANSITION_HEADER_EXEMPTIONS[transition]))
        for name, expected in EXEMPTED_HEADER_SHA256.items():
            self.assertEqual(hashlib.sha256((ROOT/'include'/'pineforge'/name).read_bytes()).hexdigest(), expected,
                             name + ' changed since the reviewed transition; land reviewed bytes and pin together')
        recorded = [{'name': name, 'oldSha256': '0'*64, 'currentSha256': sha, 'reason': 'x'}
                    for name, sha in EXEMPTED_HEADER_SHA256.items()]
        verify_exempted_header_pins(recorded)
        with self.assertRaisesRegex(RuntimeError, 'changed since the reviewed transition: native_host.hpp'):
            verify_exempted_header_pins([{'name': 'native_host.hpp', 'oldSha256': '0'*64, 'currentSha256': 'f'*64, 'reason': 'x'}])
        with self.assertRaisesRegex(RuntimeError, 'changed since the reviewed transition: native_run_spec.hpp'):
            verify_exempted_header_pins([{'name': 'native_run_spec.hpp', 'oldSha256': '0'*64, 'currentSha256': '1'*64, 'reason': 'x'}])

    def test_every_frozen_native_header_is_compared_and_exemptions_are_recorded(self):
        transition = ('engine_script_run_v13', 'engine_script_run_v15')
        self.assertEqual(set(EPOCH_TRANSITION_HEADER_EXEMPTIONS), {
            ('engine_script_run_v13', 'engine_script_run_v15'),
            ('engine_script_run_v14', 'engine_script_run_v15'),
            ('engine_script_run_v13', 'engine_script_run_v16'),
            ('engine_script_run_v14', 'engine_script_run_v16'),
            ('engine_script_run_v15', 'engine_script_run_v16')})
        self.assertEqual(set(EPOCH_TRANSITION_HEADER_EXEMPTIONS[transition]),
                         {'native_order.hpp', 'native_host.hpp', 'market_driver.hpp',
                          'execution_consumer.hpp'})
        self.assertEqual(EPOCH_TRANSITION_HEADER_EXEMPTIONS[('engine_script_run_v14','engine_script_run_v15')],
                         EPOCH_TRANSITION_HEADER_EXEMPTIONS[transition])
        self.assertEqual(EPOCH_TRANSITION_HEADER_EXEMPTIONS[('engine_script_run_v15','engine_script_run_v16')],
                         ('native_host.hpp', 'execution_consumer.hpp'))
        exempted, guarded = 'native_order.hpp', 'native_run_spec.hpp'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = self.native_headers(root/'old')
            # An unchanged header records nothing, transition or not.
            unchanged = self.native_headers(root/'unchanged')
            self.assertEqual(frozen_native_header_exemptions(old, unchanged, transition), [])
            self.assertEqual(frozen_native_header_exemptions(old, unchanged, None), [])
            # A comment-only difference is not a difference.
            commented = self.native_headers(root/'commented')
            path = commented/'pineforge'/exempted
            path.write_text(path.read_text()+'// native_order_v4 note\n')
            self.assertEqual(frozen_native_header_exemptions(old, commented, transition), [])
            # A changed exempted header under the reviewed transition is recorded.
            changed = self.native_headers(root/'changed',
                                          **{exempted: 'struct Frozen { int one; int two; };'})
            recorded = frozen_native_header_exemptions(old, changed, transition)
            self.assertEqual([item['name'] for item in recorded], [exempted])
            self.assertNotEqual(recorded[0]['oldSha256'], recorded[0]['currentSha256'])
            self.assertEqual(recorded[0]['reason'],
                             'reviewed engine_script_run_v13->engine_script_run_v15 transition')
            # The same change outside that exact transition raises.
            for other in (None, ('engine_script_run_v15', 'engine_script_run_v16')):
                with self.subTest(transition=other):
                    with self.assertRaisesRegex(RuntimeError, exempted):
                        frozen_native_header_exemptions(old, changed, other)
            # A non-exempted header still raises during the transition.
            guarded_change = self.native_headers(root/'guarded',
                                                 **{guarded: 'struct Frozen { double one; };'})
            with self.assertRaisesRegex(RuntimeError, guarded):
                frozen_native_header_exemptions(old, guarded_change, transition)

    def test_every_layout_word_is_compared_regardless_of_epoch_break(self):
        values = list(range(40))
        for epoch_break in (False, True):
            with self.subTest(epochBreak=epoch_break):
                compared = compare_layout_words('old', values, list(values), len(values),
                                                epoch_break, ['member_'])
                self.assertEqual(compared['comparedWords'], len(values))
                self.assertEqual(compared['wordCount'], len(values))
                self.assertEqual(compared['expectedEpochBreak'], epoch_break)
                self.assertEqual(compared['values'], compared['currentValues'])
                # 27 is where the retired "financial words" slice used to stop.
                for index in (0, 26, 27, 28, len(values)-1):
                    differing = list(values)
                    differing[index] += 1
                    with self.assertRaisesRegex(RuntimeError, 'differ at words: '+str(index)+'$'):
                        compare_layout_words('old', values, differing, len(values), epoch_break,
                                             ['member_'])
        with self.assertRaisesRegex(RuntimeError, 'not the expected width'):
            compare_layout_words('old', values, values[:-1], len(values), True, ['member_'])

    def test_cache_preserves_spaces_and_semicolon_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'CMakeCache.txt'
            path.write_text('// comment\nCMAKE_CXX_FLAGS:STRING=-fsanitize=address,undefined -fno-omit-frame-pointer\nCMAKE_PREFIX_PATH:PATH=/one;/two\n')
            found=read_cache(path)
            self.assertEqual(found['CMAKE_CXX_FLAGS'],'-fsanitize=address,undefined -fno-omit-frame-pointer')
            self.assertEqual(found['CMAKE_PREFIX_PATH'],'/one;/two')


if __name__=='__main__':
    unittest.main()
