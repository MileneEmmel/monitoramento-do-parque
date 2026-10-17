"""Enable/disable experimental PIR sleep or watch its safe USB status."""
import argparse
import json
import time
import re
import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--enable', action='store_true')
    modes.add_argument('--disable', action='store_true')
    parser.add_argument('--watch-seconds', type=int, default=15)
    parser.add_argument('--logs', action='store_true', help='Show only safe power/camera diagnostic messages')
    parser.add_argument('--sync-time', action='store_true', help='Set the notebook UTC time only if the ESP clock is undefined')
    args = parser.parse_args()
    if not 1 <= args.watch_seconds <= 600:
        raise SystemExit('Use de 1 a 600 segundos.')
    board = next((p for p in list_ports.comports() if p.device.upper() == args.port.upper()), None)
    if not board or (board.vid, board.pid, board.serial_number) != (6790, 21971, '5C93054388'):
        raise SystemExit('A Freenove deste projeto nao foi encontrada. Nada foi enviado.')
    command = None
    if args.enable or args.disable:
        command = ('@POWER ' + json.dumps({'enabled': args.enable}) + '\n').encode()
    device = serial.Serial()
    device.port, device.baudrate = args.port, 115200
    device.timeout, device.write_timeout = 0.3, 3
    device.dtr = device.rts = False
    acknowledged = command is None
    clock_acknowledged = not args.sync_time
    clock_command = ('@TIME ' + json.dumps({'epoch': int(time.time())}) + '\n').encode() if args.sync_time else None
    seen = False
    previous = None
    with device:
        device.reset_input_buffer()
        device.write(b'\n')
        deadline, next_query = time.monotonic() + args.watch_seconds, 0
        pending = bytearray()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_query:
                device.write(clock_command if not clock_acknowledged else command if not acknowledged else b'@POWER_STATUS\n')
                next_query = time.monotonic() + 1
            pending.extend(device.read(device.in_waiting or 1))
            while b'\n' in pending:
                line, _, pending = pending.partition(b'\n')
                if args.logs:
                    text = line.decode('utf-8', errors='replace')
                    safe = re.search(r'\bPOWER: (?:Policy self-tests PASS[^\r\x1b]*|Boot: [^\r\x1b]*|DEEP_SLEEP: [^\r\x1b]*|Sleep cancelled: [^\r\x1b]*)|\bFREENOVE: Camera \+ AI OK: [^\r\x1b]*|\bMAIN: Camera unavailable[^\r\x1b]*', text)
                    if safe:
                        print(safe.group(0), flush=True)
                for tag in (b'@TIME_CONFIG ', b'@POWER_CONFIG ', b'@POWER_STATUS '):
                    index = line.find(tag)
                    if index < 0:
                        continue
                    try:
                        status = json.loads(line[index + len(tag):])
                    except ValueError:
                        continue
                    if tag == b'@TIME_CONFIG ':
                        if status.get('ok'):
                            clock_acknowledged = True
                            print('Relogio definido se estava sem horario; registros antigos nao foram alterados.', flush=True)
                        continue
                    if tag == b'@POWER_CONFIG ':
                        if status.get('ok'):
                            acknowledged = True
                            print('Modo salvo na ESP: ' + ('PIR/deep sleep' if args.enable else 'continuo'), flush=True)
                        continue
                    seen = True
                    # Print only non-secret, power-specific fields. Ignore all other UART traffic.
                    state = {k: status.get(k) for k in ('enabled', 'phase', 'pir_level', 'sensor_validated', 'camera_started', 'detector_ready', 'wake_cause', 'clock_ok', 'last_block')}
                    if state != previous:
                        print(json.dumps(state), flush=True)
                        previous = state
            if len(pending) > 8192:
                pending.clear()
    if not acknowledged:
        raise SystemExit('Sem confirmacao de gravacao. Acorde a ESP pelo PIR ou botao RESET e tente novamente.')
    if not clock_acknowledged:
        raise SystemExit('Horario nao confirmado. Acorde a ESP e tente novamente.')
    if not seen:
        raise SystemExit('Sem status: a ESP pode estar dormindo ou executando firmware antigo. Acione o PIR e tente novamente.')


if __name__ == '__main__':
    main()
