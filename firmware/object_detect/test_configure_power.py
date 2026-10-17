import unittest
from unittest.mock import MagicMock, patch
from types import SimpleNamespace
import configure_power


class PowerUSBTests(unittest.TestCase):
    def test_status_only_does_not_change_mode_or_echo_other_uart(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.in_waiting = 1
        device.read.return_value = b'@WIFI_STATUS {"url":"private-url"}\n@POWER_STATUS {"enabled":true,"phase":"watching"}\n'
        board = SimpleNamespace(device='COM5', vid=6790, pid=21971, serial_number='5C93054388')
        with patch('sys.argv', ['configure_power.py', '--watch-seconds', '1']), \
             patch.object(configure_power.list_ports, 'comports', return_value=[board]), \
             patch.object(configure_power.serial, 'Serial', return_value=device), \
             patch.object(configure_power.time, 'monotonic', side_effect=[0, 0, 0, 0, 2]), \
             patch('builtins.print') as output:
            configure_power.main()
        self.assertFalse(device.dtr)
        self.assertFalse(device.rts)
        self.assertEqual([c.args[0] for c in device.write.call_args_list], [b'\n', b'@POWER_STATUS\n'])
        self.assertNotIn('private-url', str(output.call_args_list))

    def test_unknown_device_is_not_opened(self):
        with patch('sys.argv', ['configure_power.py', '--enable']), \
             patch.object(configure_power.list_ports, 'comports', return_value=[]), \
             patch.object(configure_power.serial, 'Serial') as serial_port, \
             self.assertRaises(SystemExit):
            configure_power.main()
        serial_port.assert_not_called()

    def test_disable_requires_ack_and_then_reads_status(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.in_waiting = 1
        device.read.side_effect = [b'@POWER_CONFIG {"ok":true}\n',
                                   b'@POWER_STATUS {"enabled":false,"phase":"continuous"}\n']
        board = SimpleNamespace(device='COM5', vid=6790, pid=21971, serial_number='5C93054388')
        with patch('sys.argv', ['configure_power.py', '--disable', '--watch-seconds', '2']), \
             patch.object(configure_power.list_ports, 'comports', return_value=[board]), \
             patch.object(configure_power.serial, 'Serial', return_value=device), \
             patch.object(configure_power.time, 'monotonic', side_effect=[0, 0, 0, 0, 1, 1, 1, 3]), \
             patch('builtins.print') as output:
            configure_power.main()
        commands = [c.args[0] for c in device.write.call_args_list]
        self.assertIn(b'@POWER {"enabled": false}\n', commands)
        self.assertEqual(commands[-1], b'@POWER_STATUS\n')
        self.assertIn('Modo salvo na ESP', str(output.call_args_list))
