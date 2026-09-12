#!/usr/bin/env python3
"""Narrow offline refusal tests for the settlement ABI tooling; no C++ execution."""
import io
from pathlib import Path
import tempfile
import tarfile
import unittest

from check_settlement_cpp_abi import ENGINE, ROOT, storage_declarations, validate_rejection
from prepare_settlement_cpp_abi_base import extract_tar, read_cache


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

    def test_generic_failure_or_missing_one_method_cannot_pass(self):
        with self.assertRaisesRegex(RuntimeError,'no recognized'):
            validate_rejection('linker error: file not found',['project_native_settlement_v1'])
        valid=f'undefined reference to `{ENGINE}project_native_settlement_v1(int)\''
        with self.assertRaisesRegex(RuntimeError,'omits expected'):
            validate_rejection(valid,['project_native_settlement_v1','project_native_settlement_scoped_v1'])

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
