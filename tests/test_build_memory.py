from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build import (ElfSection, FriendlyError, MemoryRegion, format_memory_report,
                   parse_elf_sections, parse_memory_regions)


class MemoryReportTests(unittest.TestCase):
    def test_linker_evaluated_regions_and_default_excluded(self):
        regions = parse_memory_regions("""Memory Configuration
Name Origin Length Attributes
RAM 0x20000000 0x0001c000 xrw
FLASH 0x08000000 0x00080000 xr
EMPTY 0x90000000 0x00001000 xr
*default* 0x00000000 0xffffffff

Linker script and memory map
unrelated 0x00000000 0xffffffff
""")
        self.assertEqual([r.name for r in regions], ["RAM", "FLASH", "EMPTY"])
        self.assertEqual(regions[0].length, 112 * 1024)

    def test_allocations_include_data_load_and_alignment_not_bss_load_or_debug(self):
        regions = [MemoryRegion("RAM", 0x20000000, 1024), MemoryRegion("FLASH", 0x8000000, 4096)]
        allocated = frozenset({"ALLOC"})
        loaded = frozenset({"ALLOC", "LOAD"})
        sections = [
            ElfSection(".text", 100, 0x8000000, 0x8000000, loaded),
            # 12-byte gap before initialized data in FLASH.
            ElfSection(".data", 16, 0x20000000, 0x8000070, loaded),
            ElfSection(".bss", 100, 0x20000010, 0x8000080, allocated),
            # 12-byte gap before heap/stack, counted as reserved RAM.
            ElfSection("._user_heap_stack", 64, 0x20000080, 0x8000080, allocated),
            ElfSection(".debug_info", 999999, 0, 0, frozenset({"CONTENTS", "DEBUGGING"})),
            ElfSection(".empty", 0, 0x8000800, 0x8000800, loaded),
        ]
        report = format_memory_report(regions, sections)
        ram_row = next(line for line in report.splitlines() if line.startswith("RAM "))
        flash_row = next(line for line in report.splitlines() if line.startswith("FLASH "))
        self.assertEqual(ram_row.split()[2:5], ["192", "1,024", "832"])
        self.assertEqual(flash_row.split()[2:5], ["128", "4,096", "3,968"])
        self.assertIn("999,999", report)
        for section in sections:
            self.assertIn(section.name, report)

    def test_objdump_flags_long_names_zero_sections(self):
        sections = parse_elf_sections("""file.elf: file format elf32-littlearm
Sections:
Idx Name Size VMA LMA File off Algn
  0 .data 00000010 20000000 08000070 00001000 2**2
                  CONTENTS, ALLOC, LOAD, DATA
  1 ._user_heap_stack 00000040 20000080 08000080 00002000 2**0
                  ALLOC
  2 .ARM.extab 00000000 08000080 08000080 00002000 2**0
                  CONTENTS, READONLY
""")
        self.assertEqual(len(sections), 3)
        self.assertEqual(sections[0].lma, 0x8000070)
        self.assertEqual(sections[1].flags, frozenset({"ALLOC"}))
        self.assertEqual(sections[2].size, 0)

    def test_invalid_tool_outputs_fail_clearly(self):
        for text in ("", "Memory Configuration\nNo regions"):
            with self.assertRaises(FriendlyError):
                parse_memory_regions(text)
        with self.assertRaises(FriendlyError):
            parse_elf_sections("not an ELF section listing")


if __name__ == "__main__":
    unittest.main()
