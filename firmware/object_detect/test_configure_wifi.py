import json
import unittest
from unittest.mock import MagicMock, patch
from types import SimpleNamespace

import configure_wifi


class WiFiSetupTests(unittest.TestCase):
    def setUp(self):
        ports = patch.object(configure_wifi.list_ports, 'comports', return_value=[SimpleNamespace(device='COM5',vid=6790,pid=21971,serial_number='5C93054388')])
        ports.start()
        self.addCleanup(ports.stop)

    def device(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.in_waiting = 1
        device.read.return_value = b'@WIFI_STATUS {"connected":true,"url":"http://192.168.1.25/#test-access-key"}\n'
        return device

    def test_saved_connection_never_requests_password(self):
        device = self.device()
        with patch('sys.argv', ['configure_wifi.py', '--status', '--no-browser']), \
             patch.object(configure_wifi.serial, 'Serial', return_value=device), \
             patch.object(configure_wifi.time, 'sleep'), \
             patch.object(configure_wifi.getpass, 'getpass') as password, \
             patch('builtins.print'):
            configure_wifi.main()
        password.assert_not_called()
        device.write.assert_called_once_with(b'@WIFI_STATUS\n')
        device.__exit__.assert_called_once()

    def test_password_only_sent_to_usb_not_printed(self):
        device = self.device()
        with patch('sys.argv', ['configure_wifi.py', '--no-browser']), \
             patch('builtins.input', return_value='TestNetwork'), \
             patch.object(configure_wifi.sys.stdin, 'isatty', return_value=True), \
             patch.object(configure_wifi.getpass, 'getpass', return_value='fake-test-password'), \
             patch.object(configure_wifi.serial, 'Serial', return_value=device), \
             patch.object(configure_wifi.time, 'sleep'), \
             patch('builtins.print') as output:
            configure_wifi.main()
        command = device.write.call_args_list[0].args[0]
        config = json.loads(command[len(b'@WIFI '):])
        self.assertEqual(config['password'], 'fake-test-password')
        self.assertNotIn('fake-test-password', str(output.call_args_list))

    def test_non_interactive_terminal_does_not_echo_password(self):
        with patch('sys.argv', ['configure_wifi.py']), \
             patch('builtins.input', return_value='TestNetwork'), \
             patch.object(configure_wifi.sys.stdin, 'isatty', return_value=False), \
             patch.object(configure_wifi.getpass, 'getpass') as password, \
             patch('builtins.print'), self.assertRaises(SystemExit):
            configure_wifi.main()
        password.assert_not_called()


if __name__ == '__main__':
    unittest.main()
