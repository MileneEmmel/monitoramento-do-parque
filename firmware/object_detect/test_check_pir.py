import unittest
from unittest.mock import MagicMock, patch
from types import SimpleNamespace
import check_pir


class PIRDiagnosticTests(unittest.TestCase):
    def test_only_pir_commands_and_no_unrelated_serial_output(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.in_waiting = 1
        device.read.side_effect = [
            b'@WIFI_STATUS {"url":"private-camera-url"}\n@PIR_STATUS {"ok":true,"level":0}\n',
            b'@PIR_STATUS {"ok":true,"level":1}\n',
        ]
        port = SimpleNamespace(device='COM5', vid=6790, pid=21971, serial_number='5C93054388')
        with patch('sys.argv', ['check_pir.py', '--seconds', '2']), \
             patch.object(check_pir.list_ports, 'comports', return_value=[port]), \
             patch.object(check_pir.serial, 'Serial', return_value=device), \
             patch.object(check_pir.time, 'monotonic', side_effect=[0, 0, 0, 0, 1, 1, 1, 3]), \
             patch('builtins.print') as output:
            check_pir.main()
        self.assertFalse(device.dtr)
        self.assertFalse(device.rts)
        self.assertTrue(all(c.args[0] in (b'\n', b'@PIR_STATUS\n') for c in device.write.call_args_list))
        self.assertNotIn('private-camera-url', str(output.call_args_list))
        self.assertIn('Transicoes observadas', str(output.call_args_list))

    def test_unknown_board_is_not_opened(self):
        with patch('sys.argv', ['check_pir.py']), \
             patch.object(check_pir.list_ports, 'comports', return_value=[]), \
             patch.object(check_pir.serial, 'Serial') as serial_port, \
             self.assertRaises(SystemExit):
            check_pir.main()
        serial_port.assert_not_called()
