import unittest

from dev.tools.map_plus import IMAGEBASE, _branch_target, _mask, _stub_slot


class MaskTests(unittest.TestCase):
    def test_address_encoding_instructions_lose_their_immediate(self):
        self.assertEqual(_mask(0x94001234), 0x94000000)  # BL
        self.assertEqual(_mask(0x14001234), 0x14000000)  # B
        self.assertEqual(_mask(0x90000010 | (0x1234 << 5)), 0x90000010)  # ADRP x16
        self.assertEqual(_mask(0x18000000 | (0x1234 << 5)), 0x18000000)  # LDR literal

    def test_plain_instruction_is_untouched(self):
        self.assertEqual(_mask(0xD503201F), 0xD503201F)


class BranchTargetTests(unittest.TestCase):
    def test_forward_branch(self):
        raw = b"\x00" * 0x10 + (0x94000000 | 2).to_bytes(4, "little")
        self.assertEqual(_branch_target(raw, 0x10), 0x18)

    def test_backward_branch(self):
        raw = b"\x00" * 0x10 + (0x94000000 | ((1 << 26) - 2)).to_bytes(4, "little")
        self.assertEqual(_branch_target(raw, 0x10), 0x08)


class StubSlotTests(unittest.TestCase):
    def test_decodes_the_got_slot_of_a_lazy_stub(self):
        raw = (
            (0x90000010).to_bytes(4, "little")   # adrp x16, #0
            + (0xF9400210).to_bytes(4, "little")  # ldr x16, [x16, #0]
            + (0xD61F0200).to_bytes(4, "little")  # br x16
        )
        self.assertEqual(_stub_slot(raw, 0), IMAGEBASE)

    def test_rejects_a_non_stub(self):
        self.assertIsNone(_stub_slot(b"\x1f\x20\x03\xd5" * 3, 0))


if __name__ == "__main__":
    unittest.main()
