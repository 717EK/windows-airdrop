"""Required Windows branding assets survive source packaging and match their consumers."""
from pathlib import Path
import struct
import unittest
import xml.etree.ElementTree as ET


class BrandAssetsTests(unittest.TestCase):
    def test_icon_frames_and_setup_panels(self):
        root = Path(__file__).resolve().parents[1]
        icon = (root / 'assets/brand/PulseIt.ico').read_bytes()
        self.assertEqual(struct.unpack_from('<HHH', icon), (0, 1, 7))
        sizes = []
        for index in range(7):
            width, height, _, _, planes, bits, count, offset = struct.unpack_from('<BBBBHHII', icon, 6 + 16 * index)
            sizes.append(width or 256)
            self.assertEqual(width, height)
            self.assertEqual((planes, bits), (1, 32))
            self.assertEqual(icon[offset:offset + 8], b'\x89PNG\r\n\x1a\n')
            self.assertLessEqual(offset + count, len(icon))
        self.assertEqual(sizes, [16, 24, 32, 48, 64, 128, 256])
        for name, dimensions in [('wizard.bmp', (328, 628)), ('wizard-small.bmp', (110, 110))]:
            data = (root / 'assets/installer' / name).read_bytes()
            self.assertEqual(data[:2], b'BM')
            self.assertEqual(struct.unpack_from('<ii', data, 18), dimensions)
        project = ET.parse(root / 'desktop/PulseIt.csproj').getroot()
        self.assertTrue((root / 'desktop' / project.find('.//ApplicationIcon').text.replace('\\', '/')).is_file())
        setup = (root / 'installer/PulseIt.iss').read_text()
        for setting in ('SetupIconFile', 'WizardImageFile', 'WizardSmallImageFile'):
            value = next(line.split('=', 1)[1] for line in setup.splitlines() if line.startswith(setting + '='))
            self.assertTrue((root / 'installer' / value.replace('\\', '/')).is_file())
