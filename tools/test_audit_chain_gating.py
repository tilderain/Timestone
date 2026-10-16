import struct
import unittest

from audit_chain_gating import commands


class ChainGatingParserTests(unittest.TestCase):
    def fixture(self):
        data = bytearray(256)
        struct.pack_into("<4sIIIiI", data, 0, b"CAC\0", 0x473EA35B, 1, 0, 150, 24)
        # Action-relative block offsets; an empty simple block precedes a timed block.
        struct.pack_into("<8I", data, 24, 1, 10, 0, 0, 0, 32, 0, 48)
        struct.pack_into("<4I", data, 56, 0xFFFFFFFF, 0, 0, 0)
        struct.pack_into("<4I", data, 72, 0, 2, 0, 0)
        struct.pack_into("<4I", data, 88, 32, 0, 92, 0)
        struct.pack_into("<4I", data, 104, 1, 0x81, 1, 0)
        struct.pack_into("<I", data, 120, 1234)  # Descriptor skipped by the native reader.
        struct.pack_into("<IfiiBIfii", data, 124,
                         0xFFFFFFFF, 1.0, 0, -1, 1, 2, 0.25, 3, -1)
        struct.pack_into("<4Iii", data, 164, 1, 0x99, 0, 0, 30, 30)
        return data

    def test_relative_offsets_and_packed_blend_boolean(self):
        records = list(commands(self.fixture()))
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["action"], 150)
        self.assertEqual(records[0]["values"], [0xFFFFFFFF, 1.0, 0, -1, 1, 2, 0.25, 3, -1])
        self.assertEqual(records[1]["values"], [30, 30])

    def test_invalid_header(self):
        data = self.fixture()
        data[0] = 0
        with self.assertRaises(ValueError):
            list(commands(data))

    def test_invalid_directory(self):
        data = self.fixture()
        struct.pack_into("<I", data, 8, 100)
        with self.assertRaises(ValueError):
            list(commands(data))

    def test_negative_descriptor_count(self):
        data = self.fixture()
        struct.pack_into("<i", data, 112, -1)
        with self.assertRaises(ValueError):
            list(commands(data))


if __name__ == "__main__":
    unittest.main()
