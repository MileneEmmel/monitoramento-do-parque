"""Provision only this project's ESP, over USB. Never persist or print its token."""
import argparse
import getpass
import json
import re
import sys
import time
import uuid

import serial
from serial.tools import list_ports

PUBLIC_KEY = 'sb_publishable_pV3sON9dDe2Qm8AKXGLqEQ_7PQ3P6ls'


def command(device_id, token):
    try:
        device_id = str(uuid.UUID(device_id))
    except ValueError:
        raise ValueError('ID da placa inválido.') from None
    if not re.fullmatch('[a-f0-9]{64}', token):
        raise ValueError('A chave de envio deve ter 64 caracteres hexadecimais.')
    return ('@CLOUD ' + json.dumps({'device_id': device_id, 'token': token,
                                  'publishable_key': PUBLIC_KEY}) + '\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--status', action='store_true')
    args = parser.parse_args()
    match = next((p for p in list_ports.comports() if p.device.upper() == args.port.upper()), None)
    if not match or (match.vid, match.pid, match.serial_number) != (6790, 21971, '5C93054388'):
        raise SystemExit('A Freenove deste projeto não foi encontrada. Nada foi enviado.')
    payload = None
    if not args.status:
        if not sys.stdin.isatty():
            raise SystemExit('Execute no terminal para ocultar a chave.')
        device_id = input('ID da placa cadastrado no painel: ').strip()
        token = getpass.getpass('Chave exclusiva de envio (oculta): ').strip()
        try:
            payload = command(device_id, token)
        except ValueError as error:
            raise SystemExit(str(error)) from None
        del token
    with serial.Serial(args.port, 115200, timeout=0.5) as device:
        time.sleep(3)
        device.reset_input_buffer()
        deadline = time.monotonic() + 75
        next_query = 0
        pending = bytearray()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_query:
                device.write(payload if payload else b'@CLOUD_STATUS\n')
                device.flush()
                next_query = time.monotonic() + 5
            pending.extend(device.read(device.in_waiting or 1))
            while b'\n' in pending:
                line, _, pending = pending.partition(b'\n')
                for prefix in (b'@CLOUD_CONFIG ', b'@CLOUD_STATUS '):
                    index = line.find(prefix)
                    if index < 0:
                        continue
                    try:
                        result = json.loads(line[index + len(prefix):])
                    except ValueError:
                        continue
                    if prefix == b'@CLOUD_CONFIG ':
                        if result.get('ok') is not True:
                            raise SystemExit('A placa recusou a configuração.')
                        print('Credencial gravada apenas na ESP. O envio iniciará com Wi-Fi e horário válidos.')
                    else:
                        print(json.dumps({k: result.get(k) for k in ('configured', 'syncing', 'last_http', 'last_success')}, ensure_ascii=False))
                    return
            if len(pending) > 4096:
                pending.clear()
    raise SystemExit('Sem resposta. Confira o firmware e feche o monitor serial.')


if __name__ == '__main__':
    main()
