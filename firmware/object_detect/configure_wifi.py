"""Configure ESP Wi-Fi over local USB without saving router credentials on the PC."""
import argparse
import getpass
import json
import sys
import time
import webbrowser

import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--status', action='store_true', help='Open the saved network without asking for credentials')
    parser.add_argument('--no-browser', action='store_true')
    args = parser.parse_args()
    match = next((p for p in list_ports.comports() if p.device.upper() == args.port.upper()), None)
    if not match or (match.vid, match.pid, match.serial_number) != (6790, 21971, '5C93054388'):
        raise SystemExit('A Freenove deste projeto não foi encontrada nessa porta. Nenhuma configuração foi enviada.')
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    command = None
    if not args.status:
        print('Use uma rede 2,4 GHz com senha WPA2/WPA3 modo misto (nao rede corporativa).')
        ssid = input('Nome da rede Wi-Fi (SSID): ').strip()
        if not 1 <= len(ssid.encode('utf-8')) <= 32:
            raise SystemExit('O nome da rede deve ter de 1 a 32 bytes.')
        # Fail closed if no real terminal is available: never fall back to an echoed password.
        if not sys.stdin.isatty():
            raise SystemExit('Execute este script diretamente no terminal para ocultar a senha.')
        password = getpass.getpass('Senha do Wi-Fi (nao aparece ao digitar): ')
        if not 8 <= len(password.encode('utf-8')) <= 63:
            raise SystemExit('A senha deve ter entre 8 e 63 bytes.')
        command = ('@WIFI ' + json.dumps({'ssid': ssid, 'password': password}, ensure_ascii=False) + '\n').encode('utf-8')
        del password
    try:
        device = serial.Serial(args.port, 115200, timeout=0.5)
    except serial.SerialException as error:
        raise SystemExit(f'Nao foi possivel abrir {args.port}. Feche qualquer monitor/visualizador USB e tente novamente.\n{error}')
    with device:
        # Opening the converter can reset the chip. Let firmware initialize before sending.
        time.sleep(3)
        device.reset_input_buffer()
        if command:
            device.write(command)
            device.flush()
            del command
            print('Configuracao enviada apenas pelo USB. Aguardando conexao...', flush=True)
        deadline = time.monotonic() + 75
        next_query = 0
        pending = bytearray()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_query:
                device.write(b'@WIFI_STATUS\n')
                next_query = time.monotonic() + 5
            pending.extend(device.read(device.in_waiting or 1))
            while b'\n' in pending:
                line, _, pending = pending.partition(b'\n')
                start = line.find(b'@WIFI_STATUS ')
                if start < 0:
                    continue
                try:
                    status = json.loads(line[start + len(b'@WIFI_STATUS '):])
                except ValueError:
                    continue
                if status.get('error'):
                    raise SystemExit(status['error'])
                if status.get('connected') and status.get('url'):
                    url = status['url']
                    if not url.startswith('http://') or '/#' not in url:
                        raise SystemExit('A placa retornou um endereco inesperado.')
                    print('ESP online em '+url.split('/#',1)[0]+'/. O painel e o historico funcionam sem notebook.', flush=True)
                    if not args.no_browser:
                        webbrowser.open(url)
                    return
            if len(pending) > 4096:
                pending.clear()
    raise SystemExit('Sem conexao apos 75 segundos. Confira SSID/senha, sinal e rede 2,4 GHz. Se a camera falhar, desligue a placa por 10 s e tente novamente. Nenhuma imagem sera enviada pela internet.')


if __name__ == '__main__':
    main()
