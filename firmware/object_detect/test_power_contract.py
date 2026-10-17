"""Structural safety regressions; the pure C++ policy also self-tests on the ESP."""
from pathlib import Path
import unittest

ROOT = Path(__file__).parent / 'main'


class PowerSafetyTests(unittest.TestCase):
    def test_sleep_commit_order(self):
        source = (ROOT / 'park_power.cpp').read_text(encoding='utf-8')
        steps = ['visitor_counter_freeze(true)', 'visitor_counter_flush()', 'park_cloud_request_sync()',
                 'park_cloud_pause(2000)', 'wifi_preview_sleep()', 'pir_sensor_arm_wakeup()', 'esp_deep_sleep_start()']
        positions = [source.index(step) for step in steps]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('visitor_counter_freeze(false)', source)
        self.assertIn('park_history_sleep_unlock()', source)
        self.assertIn('park_cloud_resume()', source)

    def test_sd_requires_dated_durable_snapshot(self):
        source = (ROOT / 'park_history.cpp').read_text(encoding='utf-8')
        self.assertIn('capture_locked(true)', source)
        self.assertIn('previous_e == s.entries && previous_x == s.exits', source)
        self.assertIn('baseline && dated_baseline', source)

    def test_startup_baseline_precedes_camera(self):
        source = (ROOT / 'app_main.cpp').read_text(encoding='utf-8').split('void run_detect_term()')[1]
        self.assertLess(source.index('park_history_ready()'), source.index('get_dvp_frame_cap_pipeline(false)'))
        self.assertLess(source.index('park_power_should_start_camera()'), source.index('get_dvp_frame_cap_pipeline(false)'))

    def test_power_commands_do_not_erase_storage(self):
        source = (ROOT / 'park_power.cpp').read_text(encoding='utf-8')
        self.assertNotIn('nvs_flash_erase', source)
        self.assertNotIn('format_if_mount_failed', source)
