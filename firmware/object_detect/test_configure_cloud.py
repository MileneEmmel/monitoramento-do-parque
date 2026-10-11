import json
import unittest
from configure_cloud import command


class ProvisionTests(unittest.TestCase):
    def test_valid(self):
        result = command('12345678-1234-1234-1234-123456789abc', 'a' * 64)
        self.assertLess(len(result), 1024)
        self.assertEqual(json.loads(result.decode()[7:])['token'], 'a' * 64)

    def test_reject_invalid(self):
        for device, token in [('invalid', 'a' * 64), ('12345678-1234-1234-1234-123456789abc', 'x' * 64), ('12345678-1234-1234-1234-123456789abc', 'a' * 65)]:
            with self.assertRaises(ValueError):
                command(device, token)


if __name__ == '__main__':
    unittest.main()
