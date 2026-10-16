"""Observe PIR GPIO1 over USB; never log unrelated serial data or change settings."""
import argparse
import json
import time
import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--seconds', type=int, default=60)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        raise SystemExit('Use de 1 a 600 segundos.')
    match = next((p for p in list_ports.comports() if p.device.upper() == args.port.upper()), None)
    if not match or (match.vid, match.pid, match.serial_number) != (6790, 21971, '5C93054388'):
        raise SystemExit('A Freenove deste projeto nao foi encontrada. Nada foi enviado.')
    device = serial.Serial()
    device.port, device.baudrate = args.port, 115200
    device.timeout, device.write_timeout = 0.3, 3
    device.dtr = device.rts = False
    levels = set()
    samples = changes = 0
    previous = None
    with device:
        device.reset_input_buffer()
        device.write(b'\n')
        deadline = time.monotonic() + args.seconds
        next_query = 0
        pending = bytearray()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_query:
                device.write(b'@PIR_STATUS\n')
                next_query = time.monotonic() + 0.5
            pending.extend(device.read(device.in_waiting or 1))
            while b'\n' in pending:
                line, _, pending = pending.partition(b'\n')
                index = line.find(b'@PIR_STATUS ')
                if index < 0:
                    continue
                try:
                    status = json.loads(line[index + 12:])
                except ValueError:
                    continue
                if not status.get('ok'):
                    raise SystemExit('Falha na leitura do GPIO1.')
                level = status.get('level')
                if level not in (0, 1):
                    continue
                samples += 1
                levels.add(level)
                if level != previous:
                    if previous is not None:
                        changes += 1
                    print(f'GPIO1={level} | ' + ('MOVIMENTO / saida ativa' if level else 'saida inativa'), flush=True)
                    previous = level
            if len(pending) > 8192:
                pending.clear()
    print(f'Amostras={samples}; niveis={sorted(levels)}; mudancas={changes}', flush=True)
    if not samples:
        raise SystemExit('Sem resposta PIR: confira o firmware e se a porta esta livre.')
    if levels == {0, 1}:
        print('Transicoes observadas. Compare com movimento e repouso para validar o sensor.', flush=True)
    else:
        print('Sinal constante: teste inconclusivo. Confira aquecimento, movimento e temporizacao.', flush=True)


if __name__ == '__main__':
    main()
