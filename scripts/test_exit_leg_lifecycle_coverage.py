"""Native lifecycle reflection/mirror mutation controls; no execution or compiler."""
from pathlib import Path
import unittest
import check_exit_leg_lifecycle as c
ROOT=Path(__file__).resolve().parents[1]
HEADER=(ROOT/'include/pineforge/exit_leg_lifecycle.hpp').read_text()
HASH=(ROOT/'src/source/pine_state_hash.cpp').read_text()
class Coverage(unittest.TestCase):
    def test_current(self): c.check(HEADER,HASH)
    def test_each_struct_addition_is_refused(self):
        for name in c.SCHEMA:
            with self.subTest(name=name), self.assertRaises((ValueError,SystemExit)):
                c.check(HEADER.replace('struct '+name+' {','struct '+name+' { int hidden = 0;',1),HASH)
    def test_private_addition_is_refused(self):
        with self.assertRaises(ValueError):c.check(HEADER.replace('    Definition definition_;','    int hidden_ = 0;\n    Definition definition_;'),HASH)
    def test_each_typed_fold_is_required_in_its_visitor(self):
        for name,folds in c.FOLDS.items():
            body=c.function_body(c.clean(HEADER),name)
            for fold in folds:
                with self.subTest(visitor=name,fold=fold):
                    # Locate exact unstripped visitor and change all occurrences
                    # there; a comment or unrelated helper must not cover it.
                    import re
                    m=re.search(r'\bvoid\s+'+name+r'\([^)]*\)[^{]*\{',HEADER)
                    start=m.end(); depth=1;end=start
                    while depth:
                        depth+=(HEADER[end]=='{')-(HEADER[end]=='}');end+=1
                    region=HEADER[start:end-1]
                    pattern=r'\s*'.join(map(re.escape,re.findall(r'\S',fold)))
                    altered=re.sub(pattern,' /* omitted */ ',region)
                    self.assertNotEqual(region,altered)
                    with self.assertRaises(ValueError):c.check(HEADER[:start]+altered+HEADER[end-1:]+ '\n// '+fold,HASH)
    def test_component_fold_must_be_in_pending_loop(self):
        with self.assertRaises(ValueError):c.check(HEADER,HASH.replace('o.legs.visit(f);','')+'\nvoid ignored(){o.legs.visit(f);}')
    def test_operation_addition_is_refused(self):
        with self.assertRaises(ValueError):c.check(HEADER.replace('Observe, Cancel>;','Observe, Cancel, int>;'),HASH)
    def test_each_mapping_field_and_empty_mapping_refused(self):
        import gen_pending_order_mirror as gen
        from exit_leg_reflection_schema import mapping
        actual=gen.COMPOSITE_MAP["ExitLegLifecycle"]
        try:
            for candidate in [[], *[actual[:i]+actual[i+1:] for i in range(len(actual))]]:
                gen.COMPOSITE_MAP["ExitLegLifecycle"]=candidate
                with self.assertRaises(ValueError):gen.generate()
        finally:gen.COMPOSITE_MAP["ExitLegLifecycle"]=actual
    def test_malformed_projection_and_generated_fixture_refused(self):
        import gen_pending_order_mirror as gen
        from gen_exit_lifecycle_mutations import generate, OUT
        actual=gen.COMPOSITE_MAP["ExitLegLifecycle"]
        try:
            changed=list(actual);name,kind,expr=changed[0];changed[0]=(name,kind,"0")
            gen.COMPOSITE_MAP["ExitLegLifecycle"]=changed
            with self.assertRaises(ValueError):gen.generate()
        finally:gen.COMPOSITE_MAP["ExitLegLifecycle"]=actual
        self.assertEqual(OUT.read_text(),generate())
    def test_canonical_component_cannot_be_waived(self):
        import gen_pending_order_mirror as gen
        from contextlib import redirect_stderr
        from io import StringIO
        with self.assertRaises(SystemExit),redirect_stderr(StringIO()):
            gen.classify(gen.members(),{"legs":"attempted waiver"})
    def test_fields_cannot_hide_in_public_or_after_helpers(self):
        for altered in [
            HEADER.replace("class Lifecycle {\npublic:","class Lifecycle {\npublic:\n    uint64_t hidden = 0;"),
            HEADER.replace("    static bool equal(","    uint64_t hidden_after_helpers_ = 0;\n    static bool equal("),
            HEADER.replace("    struct Exact {", "    struct Unknown { uint64_t hidden; } stored_;\n    struct Exact {"),
        ]:
            with self.assertRaises(ValueError):c.check(altered,HASH)
    def test_fields_cannot_hide_in_setter_macro(self):
        altered=HEADER.replace("set_prices(std::move(next)); return value; }", "set_prices(std::move(next)); return value; } uint64_t hidden_ = 0;")
        self.assertNotEqual(altered,HEADER)
        with self.assertRaises(ValueError):c.check(altered,HASH)
if __name__=='__main__':unittest.main()
